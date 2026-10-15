// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';

// Compatibility entry point for the shared host page. Users download export
// templates from GitHub Releases, then place them in Godot's template directory
// or select them as custom templates. Startup must not read bundled resources,
// install archives, or touch existing templates, including older Editor caches.
export async function prepareTemplates(_context: common.UIAbilityContext, _filesDir: string): Promise<void> {
}
