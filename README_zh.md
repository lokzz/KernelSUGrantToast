# KernelSU Grant Toast

<p align="center">
  <a href="/README.md">English</a> | 
  <b>简体中文</b> | 
  <a href="/README_ru.md">Русский</a>
</p>
<hr>
##### 让KernelSU像Magisk一样弹出'授予超级用户权限' Toast
### 截图
![](./mdAssets/1000023200.jpg)
![](./mdAssets/1000023198.jpg)
![](./mdAssets/1000023199.jpg)
### 功能
- 在应用提权时弹出Toast提醒
- 支持自定义提醒文本
- 支持忽略指定应用的提权提醒
### 安装
在Release中下载模块包后进入KernelSU或BakaSU中选中模块包安装即可

安装完成后需要重启生效

该模块不依赖Zygisk和MetaModule
### 兼容性
- 官方 KernelSU(32457 及以上 支持 Su 日志)
- BakaSU(原 ReSukiSU 现 4.x 版本)
- 其他保留相同 SuLog 事件 ABI 与 `KSU_IOCTL_GET_SULOG_FD` ioctl 的分支

安装时会直接探测内核是否支持 SuLog 特性 不再依赖 `KSU_KERNEL_VER_CODE`
因此不会仅因分支使用不同的版本号而被拒绝

BakaSU 将 SuLog 作为特性管理 模块会声明 `managedFeatures=sulog` 并接管
`sulogd` 读取进程 同时保留该声明以避免出现第二个读取进程
模块安装期间 管理器中的 SuLog 开关由模块接管 无法手动切换
### 原理
开启Su日志功能后 会拉起一个常驻的读取进程用于接收由内核转发的日志数据并将其写入文件
KernelSU 将该进程放在 ksud 内 BakaSU 则单独运行 `sulogd` 守护进程

该数据实时性极高 完全足够用作事件监测

安装模块后 当设备启动完成 模块将杀死原有负责日志写入的进程(即当前持有队列的进程)
并获取用于接收相关数据的文件描述符(该描述符只能被一个进程持有 故必须杀死原来的进程)接手事件处理

如果发现有Android应用被授予root权限 模块将获取该应用的相关信息 并在满足条件时弹出提醒
### 注意
由于原本负责写入日志文件的进程在设备启动完成后即被杀死 原本的Su日志将停止记录

并且为避免文件堆积 在模块启动完成后会将旧的日志文件删除

因此模块运行期间你将无法在管理器中查看Su日志数据
卸载模块后会自动恢复 SuLog 功能

(理论上也可以不杀死进程 通过监听日志文件变化实现获取信息 但这么做性能可能不佳)

### 最后
模块娱乐为主

感谢使用
