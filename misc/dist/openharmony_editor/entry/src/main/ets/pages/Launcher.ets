// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
@Entry
@Component
struct Launcher {
  @StorageLink('godotLauncherMessage') message: string = 'Godot Editor .NET';
  build() {
    Column({ space: 24 }) {
      Text(this.message).fontSize(24).fontColor('#eeeeee')
      Button('打开项目管理器').onClick(() => {
        const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
        context.startAbility({ bundleName: context.abilityInfo.bundleName, abilityName: 'EntryAbility' });
      })
    }.justifyContent(FlexAlign.Center).width('100%').height('100%').backgroundColor('#242424')
  }
}
