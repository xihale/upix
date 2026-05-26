# crossscale 项目规划书

这个项目主要功能是图片超分辨率，但是重点在于 Cross，也就是跨平台、平台间训练资源共享（拼好卡），可以组队超分，中间通过通信协议链接（可以扫二维码快速连接）
目前只考虑局域网拼资源；需要超分处分配资源，但不要预分配，因为每一个平台的资源不同，速度不同；第一次任务，需要给每一个端下发小任务，计算处理时长，计算权重，然后之后的任务分配就按这个权重来。当然，如果单机有多个显卡或者 GPU、CPU 也将其分别看作是一个端，CPU 可以设置核数，GPU 全部跑满，争取达到 100%(eff 99%+)

进入软件若无组机，则是单机模式，默认只启用本机性能最好的那个资源，比如 Nvidia 显卡；

总体上分为 PC 端 CLI 跨平台（cpp）+ apps 工作区（Android + Desktop，kt + cpp native / KMP）+ share （共用代码层）；使用 ncnn、libwebp 等库，可以参考 <https://github.com/nihui/realcugan-ncnn-vulkan>

使用 xmake 作为依赖管理、编译管理；

android 使用 MD3 界面设计，多页面，需要有日志界面

软件需要完善的日志体系，分为多个日志层级，需要颜色，可以调库

文件：

- apps - 多平台应用工作区，包括 Android 端、Desktop 端与共享 Compose UI
- cli - CLI 端代码，跨平台
- share - 共用 CPP 代码
