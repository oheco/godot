// Godot Engine contributors. SPDX-License-Identifier: MIT
import { common } from '@kit.AbilityKit';
import { readDiagnostics } from '../runtime/Diagnostics';
@Entry
@Component
struct Launcher {
  @StorageLink('godotLauncherMessage') message: string = 'Godot Editor .NET';
  @State showLog: boolean = false;
  @State logText: string = '';
  build() {
    Column({ space: 16 }) {
      Text(this.message).fontSize(18).fontColor('#eeeeee').padding({ left: 16, right: 16 })
        .copyOption(CopyOptions.LocalDevice)
      Row({ space: 12 }) {
        Button('打开项目管理器').onClick(() => {
          const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
          context.startAbility({ bundleName: context.abilityInfo.bundleName, abilityName: 'EntryAbility' });
        })
        Button('显示日志').onClick(() => {
          this.logText = readDiagnostics();
          this.showLog = !this.showLog;
        })
      }
      if (this.showLog) {
        Scroll() {
          Text(this.logText).fontSize(12).fontColor('#cccccc').textAlign(TextAlign.Start)
            .copyOption(CopyOptions.LocalDevice).width('100%')
        }.layoutWeight(1).width('100%').padding(12).backgroundColor('#111111')
      }
    }
    .justifyContent(this.showLog ? FlexAlign.Start : FlexAlign.Center)
    .width('100%').height('100%').backgroundColor('#242424').padding(12)
  }
}
