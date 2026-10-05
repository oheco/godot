# Installed-editor HDC discovery probe

This diagnostic project runs its editor plugin in the actual installed Godot UIAbility. It calls the existing oheco broker through Node 24.21.0 to run SDK26 HDC `list targets`, records stdout/stderr and exit status, and checks the live editor's setting, runnable preset and Remote Deploy menu without changing connections, forwarding rules or the daemon.

On the HarmonyOS PC used for adaptation, open this project's directory in the installed editor, or launch it with:

```sh
hdc -t <target> shell aa start -a EntryAbility -b org.oheco.godoteditor --ps godotProject /storage/Users/currentUser/dev/ohos/godot-editor/platform/openharmony/tests/device_discovery_probe
```

The plugin waits for the editor to load, records the actual runnable preset and Remote Deploy menu, prints `HDC_APP_PROBE_COMPLETE`, writes `hdc-device-probe.json` to the application's private temp directory, then removes the temporary JSON after three seconds and exits its diagnostic editor instance. Receive the JSON during that window, or read the matching engine log from the editor application's files directory. The project contains no signing material and does not change the user's game project or editor settings.

Start `oheco-broker shell serve` in the terminal before this probe. The HarmonyOS editor's **Editor Settings → Export → OpenHarmony → Use Broker** switch is enabled by default and reads `~/.oheco/broker/endpoint`; there is no port setting. The probe records the switch without changing it, executes `broker_hdc_probe.cjs` through Node for a separate read-only protocol check, and inspects the actual editor's Remote Deploy menu. A working installation reports the HDC target through the broker and shows it in the menu. The historical direct-HDC baseline has an absent socket (`ENOENT`), `ConnectUds status:-2` logs and an empty successful `list targets` result. The shipped editor does not request the additional MOUNT_HDCDEBUG_PATH permission.
