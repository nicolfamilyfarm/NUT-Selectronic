/**
 * @file selectronic-nut-poller.c
 * @brief Poll a Selectronic SP PRO JSON endpoint and publish NUT variables.
 *
 * It fetches the Selectronic local JSON feed, calculates NUT status, load and
 * runtime values, and atomically writes a dummy-ups definition file.
 *
 * The program is intended to be invoked repeatedly by the container entrypoint.
 * It performs one poll per invocation and exits with a non-zero status when a
 * poll or write fails. The entrypoint retains the previous valid data file when
 * a later poll fails.
 */

/**
 * @page selectronic_nut_poller selectronic-nut-poller - poll a Selectronic SP PRO and publish NUT values
 * @section page_synopsis SYNOPSIS
 * @code
 * SELECTRONIC_IP=INVERTER_IP SELECTRONIC_DEVICE_ID=DEVICE_ID \
 * selectronic-nut-poller
 * @endcode
 *
 * @section page_description DESCRIPTION
 * Performs one poll of the Selectronic SP PRO local HTTP JSON point endpoint
 * and atomically writes a NUT dummy-ups definition file. The container invokes
 * it repeatedly. A failed poll returns a non-zero status so the caller can
 * retain the last valid NUT data.
 *
 * @section page_environment ENVIRONMENT
 * Required: SELECTRONIC_IP and SELECTRONIC_DEVICE_ID.
 *
 * Optional: SELECTRONIC_MODEL (default SP PRO), SELECTRONIC_SERIAL,
 * NUT_DEFINITION_FILE (default /etc/nut/selectronic.dev),
 * SELECTRONIC_BATTERY_CAPACITY_KWH (default 0),
 * SELECTRONIC_RUNTIME_EFFICIENCY (default 0.9),
 * SELECTRONIC_INVERTER_CAPACITY_KW (default 5.0),
 * SELECTRONIC_SOLAR_CAPACITY_KW (default 0), SELECTRONIC_AC_VOLTAGE
 * (default 240), SELECTRONIC_BATTERY_VOLTAGE (default 0),
 * SELECTRONIC_SHUTDOWN_PERCENT (default 10), SELECTRONIC_LOW_BATTERY
 * (default 20), and SELECTRONIC_AC_MODE (default always_online).
 *
 * @section page_output OUTPUT
 * Writes standard NUT values and Selectronic telemetry to the configured
 * dummy-ups definition file using an atomic temporary-file rename.
 *
 * @section page_status EXIT STATUS
 * Returns 0 on success and non-zero when configuration, HTTP, JSON, or file
 * output processing fails.
 *
 * @section page_example EXAMPLE
 * @code
 * SELECTRONIC_IP=192.0.2.10 SELECTRONIC_DEVICE_ID=DEVICE_ID \
 * SELECTRONIC_BATTERY_CAPACITY_KWH=10 selectronic-nut-poller
 * @endcode
 */

#define _POSIX_C_SOURCE 200809L

#include <curl/curl.h>
#include <jansson.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/** Default NUT definition-file location inside the container. */
#define DEFAULT_OUTPUT_FILE "/etc/nut/selectronic.dev"

/** Maximum length of a generated endpoint URL. */
#define URL_BUFFER_SIZE 1024

/** Accumulator used by libcurl while receiving the JSON response. */
struct response_buffer {
    char *data; /**< Allocated response bytes, including a trailing NUL. */
    size_t size; /**< Number of response bytes excluding the trailing NUL. */
};

/**
 * @brief Read an environment variable or use a fallback value.
 *
 * @param name Environment variable name.
 * @param fallback Value returned when the variable is unset or empty.
 * @return The environment value or fallback. The returned pointer is owned by
 *         the process environment or points to fallback.
 */
static const char *env_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

/**
 * @brief Read a required environment variable.
 *
 * @param name Environment variable name.
 * @return The non-empty environment value.
 */
static const char *required_env(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        fprintf(stderr, "Missing required environment variable %s\n", name);
        exit(EXIT_FAILURE);
    }
    return value;
}

/**
 * @brief Parse a floating-point environment variable.
 *
 * @param name Environment variable name.
 * @param fallback Value used when unset or invalid.
 * @return Parsed value or fallback.
 */
static double env_double(const char *name, double fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    double value;

    if (text == NULL || text[0] == '\0') {
        return fallback;
    }

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        fprintf(stderr, "Invalid %s=%s; using %.3f\n", name, text, fallback);
        return fallback;
    }
    return value;
}

/**
 * @brief Append received bytes to a dynamically sized response buffer.
 *
 * @param contents Bytes supplied by libcurl.
 * @param size Size of each byte item.
 * @param item_count Number of byte items.
 * @param userdata Pointer to a response_buffer.
 * @return Number of bytes consumed, or zero on allocation failure.
 */
static size_t selectronic_curl_write_callback(const void *contents, size_t size,
                                  size_t item_count, void *userdata)
{
    struct response_buffer *buffer = userdata;
    size_t incoming = size * item_count;
    char *grown = realloc(buffer->data, buffer->size + incoming + 1);

    if (grown == NULL) {
        return 0;
    }

    memcpy(grown + buffer->size, contents, incoming);
    buffer->data = grown;
    buffer->size += incoming;
    buffer->data[buffer->size] = '\0';
    return incoming;
}

/**
 * @brief Get a numeric member from a JSON object.
 *
 * @param object JSON object.
 * @param key Member name.
 * @param fallback Value used when the member is absent or non-numeric.
 * @return JSON numeric value or fallback.
 */
static double json_number(json_t *object, const char *key, double fallback)
{
    json_t *value = json_object_get(object, key);
    return json_is_number(value) ? json_number_value(value) : fallback;
}

/**
 * @brief Calculate an estimated battery runtime in seconds.
 *
 * Positive battery power means discharge and is preferred as the demand
 * signal. While charging, the AC load is used as a conservative fallback.
 *
 * @param capacity_kwh Usable battery capacity in kWh.
 * @param state_of_charge Battery state of charge in percent.
 * @param battery_w Net battery power in watts.
 * @param load_w AC load in watts.
 * @param efficiency Usable-energy efficiency multiplier.
 * @return Estimated runtime in seconds, or one hour when insufficient data is
 *         available.
 */
static long calculate_runtime(double capacity_kwh, double state_of_charge,
                              double battery_w, double load_w,
                              double efficiency)
{
    double demand_w = battery_w > 0.0 ? battery_w : load_w;
    double seconds;

    if (capacity_kwh <= 0.0 || demand_w <= 0.0) {
        return 3600;
    }

    if (state_of_charge < 0.0) {
        state_of_charge = 0.0;
    }
    if (efficiency <= 0.0 || efficiency > 1.0) {
        efficiency = 0.9;
    }

    seconds = capacity_kwh * 1000.0 * (state_of_charge / 100.0) * efficiency
              / demand_w * 3600.0;
    return seconds < 0.0 ? 0 : (long)(seconds + 0.5);
}

/**
 * @brief Atomically write a NUT dummy-ups definition file.
 *
 * @param path Destination file path.
 * @param model NUT model name.
 * @param serial Device serial number.
 * @param status NUT status string.
 * @param soc Battery state of charge.
 * @param runtime Estimated runtime in seconds.
 * @param ac_voltage Nominal AC input/output voltage.
 * @param battery_voltage Configured nominal battery voltage.
 * @param load_w Current load in watts.
 * @param inverter_capacity_kw Inverter output rating in kW.
 * @param battery_w Battery power in watts.
 * @param grid_w Grid/generator power in watts.
 * @param solar_w Solar inverter power in watts.
 * @param solar_capacity_kw Solar capacity in kW.
 * @param fault_code Selectronic fault code.
 * @param timestamp Selectronic feed timestamp.
 * @param items Selectronic JSON items object containing additional telemetry.
 * @return 0 on success, -1 on failure.
 */
static int write_definition(const char *path, const char *model,
                            const char *serial, const char *status,
                            double soc, long runtime, double ac_voltage,
                            double battery_voltage,
                            double load_w, double inverter_capacity_kw,
                            double battery_w, double grid_w, double solar_w,
                            double solar_capacity_kw, double fault_code,
                            long long timestamp, json_t *items)
{
    char temporary[URL_BUFFER_SIZE];
    FILE *file;
    double load_percent;
    double nominal_w;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                 (long)getpid()) >= (int)sizeof(temporary)) {
        fprintf(stderr, "Temporary output path is too long\n");
        return -1;
    }

    file = fopen(temporary, "w");
    if (file == NULL) {
        perror("fopen NUT definition");
        return -1;
    }

    if (inverter_capacity_kw <= 0.0) {
        inverter_capacity_kw = 1.0;
    }
    load_percent = load_w < 0.0 ? 0.0 : load_w / (inverter_capacity_kw * 10.0);
    nominal_w = inverter_capacity_kw * 1000.0;

    fprintf(file, "ups.mfr: Selectronic\n");
    fprintf(file, "ups.model: %s\n", model);
    fprintf(file, "ups.status: %s\n", status);
    fprintf(file, "battery.charge: %.1f\n", soc);
    fprintf(file, "battery.runtime: %ld\n", runtime);
    fprintf(file, "battery.voltage: %.1f\n", battery_voltage);
    fprintf(file, "input.voltage: %.1f\n", ac_voltage);
    fprintf(file, "output.voltage: %.1f\n", ac_voltage);
    fprintf(file, "ups.load: %.1f\n", load_percent);
    fprintf(file, "ups.realpower: %.1f\n", load_w);
    fprintf(file, "ups.realpower.nominal: %.1f\n", nominal_w);
    fprintf(file, "ups.loadhigh: 90\n");
    fprintf(file, "ups.serial: %s\n", serial);
    fprintf(file, "device.serial: %s\n", serial);
    fprintf(file, "selectronic.battery_w: %.1f\n", battery_w);
    fprintf(file, "selectronic.grid_w: %.1f\n", grid_w);
    fprintf(file, "selectronic.solar_w: %.1f\n", solar_w);
    fprintf(file, "selectronic.solar_capacity_kw: %.1f\n", solar_capacity_kw);
    fprintf(file, "selectronic.battery_in_wh_today: %.3f\n",
            json_number(items, "battery_in_wh_today", 0.0));
    fprintf(file, "selectronic.battery_in_wh_total: %.3f\n",
            json_number(items, "battery_in_wh_total", 0.0));
    fprintf(file, "selectronic.battery_out_wh_today: %.3f\n",
            json_number(items, "battery_out_wh_today", 0.0));
    fprintf(file, "selectronic.battery_out_wh_total: %.3f\n",
            json_number(items, "battery_out_wh_total", 0.0));
    fprintf(file, "selectronic.grid_in_wh_today: %.3f\n",
            json_number(items, "grid_in_wh_today", 0.0));
    fprintf(file, "selectronic.grid_in_wh_total: %.3f\n",
            json_number(items, "grid_in_wh_total", 0.0));
    fprintf(file, "selectronic.grid_out_wh_today: %.3f\n",
            json_number(items, "grid_out_wh_today", 0.0));
    fprintf(file, "selectronic.grid_out_wh_total: %.3f\n",
            json_number(items, "grid_out_wh_total", 0.0));
    fprintf(file, "selectronic.load_wh_today: %.3f\n",
            json_number(items, "load_wh_today", 0.0));
    fprintf(file, "selectronic.load_wh_total: %.3f\n",
            json_number(items, "load_wh_total", 0.0));
    fprintf(file, "selectronic.solar_wh_today: %.3f\n",
            json_number(items, "solar_wh_today", 0.0));
    fprintf(file, "selectronic.solar_wh_total: %.3f\n",
            json_number(items, "solar_wh_total", 0.0));
    fprintf(file, "selectronic.shunt_w: %.3f\n",
            json_number(items, "shunt_w", 0.0));
    fprintf(file, "selectronic.gen_status: %.0f\n",
            json_number(items, "gen_status", 0.0));
    fprintf(file, "selectronic.fault_ts: %.0f\n",
            json_number(items, "fault_ts", 0.0));
    fprintf(file, "selectronic.fault_code: %.0f\n", fault_code);
    fprintf(file, "selectronic.timestamp: %lld\n", timestamp);
    fprintf(file, "device.mfr: Selectronic\n");
    fprintf(file, "device.model: %s\n", model);

    if (fclose(file) != 0 || rename(temporary, path) != 0) {
        perror("write NUT definition");
        unlink(temporary);
        return -1;
    }
    return 0;
}

/**
 * @brief Poll Selectronic once and update the NUT definition file.
 *
 * @return EXIT_SUCCESS on success, EXIT_FAILURE on any poll or write error.
 */
int main(void)
{
    const char *ip = required_env("SELECTRONIC_IP");
    const char *device_id = required_env("SELECTRONIC_DEVICE_ID");
    const char *model = env_or_default("SELECTRONIC_MODEL", "SP PRO");
    const char *serial = env_or_default("SELECTRONIC_SERIAL", "");
    const char *output = env_or_default("NUT_DEFINITION_FILE", DEFAULT_OUTPUT_FILE);
    const char *ac_mode = env_or_default("SELECTRONIC_AC_MODE", "always_online");
    char url[URL_BUFFER_SIZE];
    struct response_buffer response = {0};
    CURL *curl;
    CURLcode curl_result;
    json_error_t json_error;
    json_t *root;
    json_t *items;
    double soc, load_w, battery_w, grid_w, solar_w, fault_code;
    double capacity_kwh, efficiency, inverter_kw, solar_kw, ac_voltage;
    double battery_voltage;
    double timestamp_value;
    long long timestamp;
    long now;
    char status[16] = "OL";
    long runtime;

    if (snprintf(url, sizeof(url),
                 "http://%s/cgi-bin/solarmonweb/devices/%s/point",
                 ip, device_id) >= (int)sizeof(url)) {
        fprintf(stderr, "Selectronic URL is too long\n");
        return EXIT_FAILURE;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "Unable to initialize libcurl\n");
        return EXIT_FAILURE;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, selectronic_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (curl_result != CURLE_OK) {
        fprintf(stderr, "Selectronic request failed: %s\n",
                curl_easy_strerror(curl_result));
        free(response.data);
        return EXIT_FAILURE;
    }

    root = json_loads(response.data, 0, &json_error);
    free(response.data);
    if (root == NULL) {
        fprintf(stderr, "Invalid Selectronic JSON at line %d: %s\n",
                json_error.line, json_error.text);
        return EXIT_FAILURE;
    }
    items = json_object_get(root, "items");
    if (!json_is_object(items)) {
        fprintf(stderr, "Selectronic JSON has no items object\n");
        json_decref(root);
        return EXIT_FAILURE;
    }

    soc = json_number(items, "battery_soc", -1.0);
    load_w = json_number(items, "load_w", -1.0);
    battery_w = json_number(items, "battery_w", 0.0);
    grid_w = json_number(items, "grid_w", 0.0);
    solar_w = json_number(items, "solarinverter_w", 0.0);
    fault_code = json_number(items, "fault_code", 0.0);
    timestamp_value = json_number(root, "now", json_number(items, "timestamp", 0.0));
    timestamp = (long long)timestamp_value;
    now = (long)time(NULL);
    capacity_kwh = env_double("SELECTRONIC_BATTERY_CAPACITY_KWH", 0.0);
    efficiency = env_double("SELECTRONIC_RUNTIME_EFFICIENCY", 0.9);
    inverter_kw = env_double("SELECTRONIC_INVERTER_CAPACITY_KW", 5.0);
    solar_kw = env_double("SELECTRONIC_SOLAR_CAPACITY_KW", 0.0);
    ac_voltage = env_double("SELECTRONIC_AC_VOLTAGE", 240.0);
    battery_voltage = env_double("SELECTRONIC_BATTERY_VOLTAGE", 0.0);

    if (soc < 0.0 || load_w < 0.0 || timestamp <= 0 || now - timestamp > 30) {
        strcpy(status, "OB LB");
    } else if (fault_code != 0.0) {
        strcpy(status, "OB");
    } else if (soc <= env_double(
                   "SELECTRONIC_SHUTDOWN_PERCENT",
                   env_double("SELECTRONIC_CRITICAL_BATTERY", 10.0))) {
        strcpy(status, "OB LB");
    } else if (soc <= env_double("SELECTRONIC_LOW_BATTERY", 20.0)) {
        strcpy(status, "OL LB");
    } else if (strcmp(ac_mode, "grid_w_nonzero") == 0 && grid_w == 0.0) {
        strcpy(status, "OB");
    } else if (strcmp(ac_mode, "always_online") != 0
               && strcmp(ac_mode, "grid_w_nonzero") != 0) {
        fprintf(stderr, "Unsupported SELECTRONIC_AC_MODE=%s\n", ac_mode);
        json_decref(root);
        return EXIT_FAILURE;
    }

    runtime = calculate_runtime(capacity_kwh, soc, battery_w, load_w, efficiency);
    if (strstr(status, "LB") != NULL && soc <= 10.0) {
        runtime = 300;
    } else if (strstr(status, "LB") != NULL) {
        runtime = 900;
    } else if (strcmp(status, "OB") == 0) {
        runtime = 0;
    }

    {
        int result = write_definition(output, model, serial, status, soc,
                                      runtime, ac_voltage, battery_voltage,
                                      load_w, inverter_kw, battery_w, grid_w,
                                      solar_w, solar_kw, fault_code, timestamp,
                                      items);
        json_decref(root);
        return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
}
