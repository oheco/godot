# Mouse and touchpad input

The Editor and game templates share this input path. ArkUI callbacks enqueue fixed-layout host events; Godot input and GUI handling run on the engine thread. Host ABI 4 is unchanged: event kinds 3 and 4 add pan and magnify semantics using the existing delta/factor fields.

## Units and device semantics

- Cursor coordinates in the ArkTS event are component-local vp. Convert positions to physical pixels once.
- MMI mouse-wheel axes are libinput angular increments multiplied by the system's configured scroll rows. ArkUI forwards these raw values through its dynamic ArkTS getters. Normalize wheel magnitude by `15 degrees * 3 baseline rows = 45`; do not divide by `scrollStep` again. This preserves system preferences and fractional/high-resolution increments.
- The 15-degree quantity is the platform's conventional wheel unit, not a claim that all physical devices have identical detents. Wheel-event magnitude is not multiplied by display density.
- Wheel direction is carried by Godot WHEEL_UP/DOWN/LEFT/RIGHT. Press and release carry the same fractional magnitude. Their masks include/exclude the transient wheel button while preserving physical held buttons.
- Touchpad finger axes are incremental physical-pixel distances, with system direction/preferences already applied. Convert to logical distances, then use Godot's desktop precise-scroll gain (`0.03`) to produce one two-dimensional `InputEventPanGesture`. Do not invert natural scrolling again, split diagonals into wheel pulses, quantize tiny movements, or apply a mouse-wheel row factor.
- BEGIN may contain the first real increment. Process BEGIN and UPDATE, and accept legacy NONE samples. END/CANCEL clear state and do not replay a terminal payload.
- Touchpad pinch scale is cumulative from gesture start. Convert it to an incremental ratio (`current / previous`) for `InputEventMagnifyGesture`, keep devices independent, and clear its state on END/CANCEL or loss of focus.
- Ordinary pointer motion uses pixel differences between absolute positions. Hardware `rawDeltaX/Y` is used only in captured mode; API 26 explicitly defines it as hardware motion rather than screen pixels.
- Native pressed-button snapshots recover releases missed outside the surface. Focus/surface loss explicitly releases held physical buttons and resets gesture state. Double-click distance uses logical pixels, and a drag invalidates the previous click sequence.

The built-in Godot controls retain their own page/line scrolling semantics. This conversion supplies consistent event magnitude; it does not force every control to scroll the same number of pixels.

## Source contract

The API 26 ArkTS declaration describes axis getters as vp, but official ArkUI source forwards the raw values. The implementation above follows the source path and the native SDK's distinction between wheel angles and touchpad pixels. The following pinned upstream snapshots document that path; they do not establish byte identity with every vendor's runtime:

- [MMI wheel normalization and preferences](https://github.com/openharmony/multimodalinput_input/blob/7f674c11dee34d4c158827ca8f0e8db3b12a1d4b/service/mouse_event_normalize/src/mouse_transform_processor.cpp#L859).
- [ArkUI coordinate conversion and event construction](https://github.com/openharmony/arkui_ace_engine/blob/21b150e7bb1d5308846f8e5be8ef0482fb433315/frameworks/bridge/declarative_frontend/engine/jsi/nativeModule/arkts_native_common_bridge.cpp#L11483).
- [Raw ArkTS axis getters](https://github.com/openharmony/arkui_ace_engine/blob/21b150e7bb1d5308846f8e5be8ef0482fb433315/frameworks/bridge/declarative_frontend/engine/jsi/nativeModule/arkts_utils.cpp#L3941).
- [Platform wheel, line-height and baseline constants](https://github.com/openharmony/arkui_ace_engine/blob/21b150e7bb1d5308846f8e5be8ef0482fb433315/frameworks/core/components_ng/event/event_constants.h#L197).
- [Incremental offset conversion and direction](https://github.com/openharmony/arkui_ace_engine/blob/21b150e7bb1d5308846f8e5be8ef0482fb433315/frameworks/core/event/axis_event.cpp#L88).
- [Cumulative libinput pinch contract](https://github.com/openharmony/third_party_libinput/blob/4db5dcfe4bc75ef983b15314c6a2103bb555052c/src/libinput.h#L2039).

ArkUI coasting is a distinct native-node callback, not part of the supplied ArkTS `onAxisEvent` declaration. This implementation does not synthesize its own inertia. Native coasting registration and non-pointer gestures such as three-finger workspace operations are not claimed by this path.

## Focused validation

Run the real ArkTS conversion/model tests with the installed SDK compiler:

```sh
node platform/openharmony/tests/test_touch_input.cjs --compiler /path/to/sdk/typescript/lib/typescript.js
```

Use the native Input/Viewport pointer fixture and native log-selection fixture against a matching signed engine build. These validate event conversion and GUI behavior without simulating input into another application.

For device acceptance, use an independent project and check:

1. Output drag selection inside the text region leaves the scroll value unchanged. Above/below the text region, including beyond the APP window, horizontal movement continues changing the endpoint. Repeat at both scroll limits, in padding/gaps, and with wrapped text. Release/focus loss stops updates and preserves the configured selection behavior.
2. Mouse single/fractional/repeated wheel samples in Output, scene/filesystem trees, inspector containers, item lists and the script editor. Check both directions, horizontal/Shift scrolling, held physical buttons, and at least two system scroll-row preferences.
3. Touchpad slow/tiny/fast and diagonal two-finger scrolling, both natural-scroll settings, pinch, terminal samples and restarting after cancellation/focus loss. A micro-gesture must not introduce a full wheel tick.
4. Visible/confined pointer movement at two display densities, captured raw movement, ordinary/right/middle/back/forward clicks, double-clicks, dragging, release outside the surface, and returning from another application.

Existing C# build/export/debug/remote-debug/automatic-signing acceptance is reused unless changes or failures affect those paths.
