# openHAB example configuration

Tested with openHAB 5.2 and the Home Assistant binding (MQTT discovery), firmware 1.38.16.

| File | Content |
|---|---|
| [beca.items](beca.items) | Items for three example thermostats (office, kitchen, living room) |
| [beca_setpoint_stepper.yaml](beca_setpoint_stepper.yaml) | Custom widget: setpoint with −/+ buttons, current temperature, mode and state |
| [page_example.yaml](page_example.yaml) | Page with target and scheduled temperatures of all thermostats |

## Things
The thermostats appear in the inbox as Home Assistant devices. Add them with the suggested thing id (`beca-office` → `becaOffice`).  
Channels: `thermostat#current-temperature`, `thermostat#temperature`, `thermostat#mode`, `thermostat#preset-mode`, `thermostat#action` (only with a heating state), `schedules_temperature`, `locked`, `floor_temperature` (only with a floor sensor).

The binding caches every discovery topic it has seen. If a thing still lists old topics (e.g. after updating from an older firmware or renaming a device), remove the thing and the inbox entry and restart the binding in the Karaf console:
```
bundle:list | grep -i "home assistant"
bundle:restart <id>
```

## Items
Adjust the broker thing id (`local`) and the location groups (`room_*`), then paste the items into Developer Tools → Add Items from Textual Definition, or put the file into `$OPENHAB_CONF/items`.

- **Heating control** (`heat` / `off`) is a Switch via the SCRIPT ECMAScript profile and needs the JavaScript Scripting add-on. The MAP profile only transforms device → item, commands would not reach the thermostat.
- **Operation mode** and **State** are String items with options (Auto / Manual, Heating / Idle).
- Avoid the semantic tags `Switch` / `Power` on String items: Main UI then shows a toggle that sends `ON` / `OFF`, which the thermostat ignores.
- Setpoints are limited to 5–35 °C in steps of 0.5 by the state description.

## Widget
Developer Tools → Widgets → `+`, paste [beca_setpoint_stepper.yaml](beca_setpoint_stepper.yaml).  
The widget expects the item names `<prefix>_Temperature`, `<prefix>_OperationMode` and `<prefix>_State` next to the setpoint item `<prefix>_TargetTemperature` or `<prefix>_ScheduledTemperature`.
```yaml
- component: widget:beca_setpoint_stepper
  config:
    item: becaOffice_TargetTemperature
    title: Office
```
The value between the buttons uses the item's state pattern (`22.0 °C`); the built-in `oh-stepper` drops the trailing `.0` and ignores the step of the state description.
