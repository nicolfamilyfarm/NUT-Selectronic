# Selectronic SP PRO NUT container

This container publishes a Selectronic SP PRO inverter as a Network UPS Tools
(NUT) network UPS. It polls the inverter's local HTTP JSON API and serves the
result through a NUT `dummy-ups` instance named `selectronic`.

The container is suitable for Podman or Docker and can run alongside another
NUT server, provided that it is published on a different host port.

## How it works

At startup, the container reads the device metadata endpoint once to obtain
the serial number, inverter rating, and firmware version. It then polls the
device point endpoint at the configured interval, calculates NUT status and
runtime values, and updates the NUT data file atomically.

The container listens on TCP port 3493 internally. Publish it on an available
host port, such as 3494, when the host already uses TCP port 3493 for NUT.

## Requirements

- Podman or Docker
- Network access from the container to the Selectronic device
- A Selectronic local web interface exposing the `/cgi-bin/solarmonweb` API
- The device IP address and device ID
- A host port available for NUT clients

The device ID is the identifier in the device API URL:

```text
http://DEVICE_IP/cgi-bin/solarmonweb/devices/DEVICE_ID
```

## Build

Run this command from the directory containing `Containerfile`:

```sh
podman build -t selectronic-nut:latest -f Containerfile .
```

For Docker, use the equivalent `docker build` command and image name.

## Configuration

The following variables are supported by the container:

| Variable | Required | Default | Description |
|---|---:|---:|---|
| `SELECTRONIC_IP` | yes | — | IP address or resolvable hostname of the inverter. |
| `SELECTRONIC_DEVICE_ID` | yes | — | Device identifier used by the Selectronic API. |
| `SELECTRONIC_MODEL` | no | `SP PRO` | Model label. Firmware is appended automatically when available. |
| `SELECTRONIC_BATTERY_CAPACITY_KWH` | no | `0` | Usable battery capacity used for runtime estimation. |
| `SELECTRONIC_SOLAR_CAPACITY_KW` | no | `0` | Solar capacity exposed as `selectronic.solar_capacity_kw`. |
| `SELECTRONIC_AC_VOLTAGE` | no | `240` | Nominal input and output voltage reported to NUT. |
| `SELECTRONIC_BATTERY_VOLTAGE` | no | `0` | Nominal battery voltage reported to NUT. The point feed does not provide a measured voltage. |
| `SELECTRONIC_RUNTIME_EFFICIENCY` | no | `0.9` | Runtime efficiency multiplier, from 0 to 1. |
| `SELECTRONIC_SHUTDOWN_PERCENT` | no | `10` | Battery percentage at or below which NUT reports `OB LB`. |
| `SELECTRONIC_LOW_BATTERY` | no | `20` | Battery percentage at or below which NUT reports `OL LB`. |
| `SELECTRONIC_AC_MODE` | no | `always_online` | Use `grid_w_nonzero` only when non-zero `grid_w` reliably means AC input is present. |
| `POLL_INTERVAL` | no | `5` | Seconds between API polls. |
| `NUT_USER` | no | `nutmon` | NUT account created by the container. |
| `NUT_PASSWORD` | no | `change-me` | Password for the NUT account. Set a strong value. |

Serial number, inverter rating, and firmware are read from the metadata feed.
Battery capacity, solar capacity, nominal AC voltage, nominal battery voltage,
and the exact model label must be supplied when they are not available from the
device API.

## Run with Podman

Replace the placeholder values with values for the target installation. Choose
a host port that is not already used by another NUT server.

```sh
podman run -d \
  --name selectronic-nut \
  --restart=unless-stopped \
  -p HOST_NUT_PORT:3493 \
  -e SELECTRONIC_IP=DEVICE_IP \
  -e SELECTRONIC_DEVICE_ID=DEVICE_ID \
  -e SELECTRONIC_MODEL='MODEL_LABEL' \
  -e SELECTRONIC_BATTERY_CAPACITY_KWH=BATTERY_CAPACITY \
  -e SELECTRONIC_SOLAR_CAPACITY_KW=SOLAR_CAPACITY \
  -e SELECTRONIC_AC_VOLTAGE=AC_VOLTAGE \
  -e SELECTRONIC_BATTERY_VOLTAGE=BATTERY_VOLTAGE \
  -e SELECTRONIC_SHUTDOWN_PERCENT=SHUTDOWN_PERCENT \
  -e NUT_USER=nutmon \
  -e NUT_PASSWORD='STRONG_PASSWORD' \
  selectronic-nut:latest
```

For example, if the host publishes the container on port 3494, NUT clients
connect to `selectronic@HOSTNAME_OR_IP:3494`.

## Test the service

From a system with NUT client utilities installed:

```sh
upsc selectronic@HOSTNAME_OR_IP:HOST_NUT_PORT
```

The service should return values including `ups.status`, `ups.model`,
`battery.charge`, `battery.runtime`, `ups.load`, and `ups.realpower`.

Useful container diagnostics:

```sh
podman ps --filter name=selectronic-nut
podman logs selectronic-nut
podman exec selectronic-nut upsc selectronic@127.0.0.1
```

## Configure NUT Web Monitor

Add the published endpoint to the monitor's NUT hosts configuration. The exact
file location depends on the monitor installation; the entry is:

```text
MONITOR selectronic@HOSTNAME_OR_IP:HOST_NUT_PORT "Selectronic SP PRO"
```

Reload or restart the web monitor after changing its hosts configuration.

## Configure NUT clients and shutdown

The container creates the configured NUT account with the `upsmon primary`
role. A client using this account can monitor the UPS over the published port.
For a client that should participate in coordinated shutdown, configure its
`upsmon.conf` with a matching account and the `primary` role as appropriate for
the NUT topology:

```ini
MONITOR selectronic@NUT_SERVER:HOST_NUT_PORT 1 nutmon STRONG_PASSWORD primary
```

The `SELECTRONIC_SHUTDOWN_PERCENT` setting only changes when the device reports
`OB LB`; it does not directly power off the inverter. A client shuts down only
when its own `upsmon` policy, timers, and privileges permit it.

## Reported variables

Standard NUT values include:

- `ups.status`, `ups.load`, `ups.realpower`, and `ups.realpower.nominal`
- `input.voltage` and `output.voltage`
- `battery.charge`, `battery.runtime`, and `battery.voltage`
- `ups.serial`, `device.serial`, `ups.model`, and `device.model`

Selectronic-specific values include live battery, grid, and solar power plus
energy totals and fault information, including `selectronic.battery_w`,
`selectronic.grid_w`, `selectronic.solar_w`, `selectronic.fault_code`, and the
`*_wh_today` and `*_wh_total` fields from the point feed.

Solar power is exposed as a NUT variable. It is not represented as a separate
topology flow by all NUT web monitors.

## Runtime calculation

When capacity and demand are available, estimated runtime is calculated as:

```text
capacity_kwh × 1000 × state_of_charge / 100 × efficiency
----------------------------------------------------------------
                         demand_w
```

Battery discharge power is used as demand when positive. Otherwise, AC load is
used. If capacity or demand is unavailable, the service reports a one-hour
fallback runtime.

## Upgrade or remove

To upgrade, build the new image, replace the container using the same variables,
and verify it with `upsc`:

```sh
podman build -t selectronic-nut:latest -f Containerfile .
podman rm -f selectronic-nut
# Re-run the documented podman run command.
upsc selectronic@HOSTNAME_OR_IP:HOST_NUT_PORT
```

To remove the service:

```sh
podman rm -f selectronic-nut
```

Also remove its `MONITOR` entry from any NUT web monitor configuration.

## Troubleshooting

**The container exits immediately:** check `podman logs selectronic-nut`. The
most common causes are an incorrect IP, device ID, unavailable API, or invalid
metadata response.

**`upsc` reports `Unknown UPS`:** verify the host port, container status, and
that the UPS name is exactly `selectronic`:

```sh
upsc selectronic@HOSTNAME_OR_IP:HOST_NUT_PORT
```

**Values remain stale:** inspect the logs and test both API endpoints from the
container network. The last valid NUT data is retained when a later poll fails.

**Runtime is inaccurate:** verify usable battery capacity, battery voltage,
load, and `SELECTRONIC_RUNTIME_EFFICIENCY`. The capacity value should represent
usable energy rather than the battery's nominal nameplate capacity.

**Another NUT server already uses port 3493:** publish this container on a
different host port and use that port in every NUT client and monitor entry.

## License and contribution

The project consists of the `Containerfile`, `entrypoint.sh`, and
`selectronic-nut-poller.c`. Contributions should preserve the documented
environment-variable interface and validate changes with a clean image build
and an `upsc` query against a test device or fixture.
