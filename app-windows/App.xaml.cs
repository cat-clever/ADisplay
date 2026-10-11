using Microsoft.UI.Xaml;

namespace ADisplay.Windows;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        // 启动日志最早的那部分已经由 StartupLog 的模块初始化器写完了（比这里还早），
        // 这里接着往下串节点。节点成对出现，日志断在哪个「进入」上就是死在哪一步。
        StartupLog.Enter("App 构造函数");

        // WinUI 的未处理异常挂在 Application 实例上，只有这时才挂得上，
        // 也必须在 InitializeComponent 之前挂 —— XAML 解析失败正需要它来记。
        StartupLog.HookXamlExceptions(this);

        StartupLog.Enter("App.InitializeComponent()");
        InitializeComponent();
        StartupLog.Leave("App.InitializeComponent()");

        StartupLog.Leave("App 构造函数");
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        StartupLog.Enter("OnLaunched");

        _window = new MainWindow();
        _window.Activate();

        StartupLog.Leave("OnLaunched");
    }
}
