#include "pch.h"
#include "resource.h"
#include <win32app/XamlWin32Helpers.h>
#include <win32app/win32_app_helpers.h>
#include <win32app/reference_waiter.h>

struct AppWindow : public std::enable_shared_from_this<AppWindow>
{
    AppWindow(winrt::Windows::System::DispatcherQueueController queueController, bool rightClickLaunch = false) :
        m_queueController(std::move(queueController)),
        m_rightClickLaunch(rightClickLaunch),
        m_xamlManager(winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread())
    {
    }

    ~AppWindow()
    {
    }

    LRESULT Create()
    {
        using namespace winrt::Windows::UI::Xaml;
        using namespace winrt::Windows::UI::Xaml::Controls;
        using namespace winrt::Windows::UI::Xaml::Input;

        // WindowsXamlManager must be used if multiple islands are created on the thread or in the process.
        // It must be constructed before the first DesktopWindowXamlSource.
        m_xamlSource = winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource();

        auto interop = m_xamlSource.as<IDesktopWindowXamlSourceNative>();
        THROW_IF_FAILED(interop->AttachToWindow(m_window.get()));
        THROW_IF_FAILED(interop->get_WindowHandle(&m_xamlSourceWindow));

        auto content = LoadXamlResource(nullptr, IDR_APP_XAML);

        m_xamlSource.Content(content);

        m_status = content.as<FrameworkElement>().FindName(L"Status").as<TextBlock>();

        m_rootChangedRevoker = content.XamlRoot().Changed(winrt::auto_revoke, [this](auto&& sender, auto&& args)
        {
            auto scale = sender.RasterizationScale();
            auto visible = sender.IsHostVisible();

            m_status.Text(std::to_wstring(scale));
        });

        m_pointerPressedRevoker = content.PointerPressed(winrt::auto_revoke, [](auto&& sender, auto&& args)
        {
            const bool isRightClick = args.GetCurrentPoint(sender.as<UIElement>()).Properties().IsRightButtonPressed();

            BroadcastAsync([](auto&& appWindow)
            {
                appWindow.m_status.Text(L"Broadcast");
            });

            StartThreadAsync([isRightClick](auto&& queueController)
            {
                std::make_shared<AppWindow>(std::move(queueController), isRightClick)->Show(SW_SHOWNORMAL);
            });
        });

        return 0;
    }

    LRESULT Size(WORD dx, WORD dy)
    {
        SetWindowPos(m_xamlSourceWindow, nullptr, 0, 0, dx, dy, SWP_SHOWWINDOW);
        return 0;
    }

    LRESULT Destroy()
    {
        ReportRemoved();

        // Since the xaml rundown is async and requires message dispatching,
        // start its run down here while the message loop is still running.
        m_xamlSource.Close();

        [](auto that) -> winrt::fire_and_forget
        {
            auto delayedRelease = std::move(that->m_selfRef);
            co_await that->m_queueController.ShutdownQueueAsync();
        }(this);

        return 0;
    }

    void Show(int nCmdShow)
    {
        ReportAdded();

        win32app::create_top_level_window_for_xaml(*this, L"Win32XamlAppWindow", L"Win32 Xaml App");
        m_selfRef = shared_from_this();
        ShowWindow(m_window.get(), nCmdShow);
    }

    winrt::Windows::System::DispatcherQueue DispatcherQueue() const
    {
        return m_queueController.DispatcherQueue();
    }

    template <typename Lambda>
    static winrt::fire_and_forget StartThreadAsync(Lambda fn)
    {
        auto queueController = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
        co_await queueController.DispatcherQueue();
        fn(std::move(queueController));
    }

    void ReportAdded()
    {
        auto lock = std::lock_guard<std::mutex>(m_appWindowLock);
        AppWindow::m_appWindows.emplace_back(weak_from_this());

        m_appRefHolder.emplace(m_appThreadsWaiter.take_reference());
    }

    void ReportRemoved()
    {
        auto lock = std::lock_guard<std::mutex>(m_appWindowLock);
        m_appWindows.erase(std::find_if(m_appWindows.begin(), m_appWindows.end(), [&](auto&& weakOther)
        {
            if (auto strong = weakOther.lock())
            {
                return strong.get() == this;
            }
            return false;
        }));

        m_appRefHolder.reset();
    }

    static std::vector<std::shared_ptr<AppWindow>> GetAppWindows()
    {
        std::vector<std::shared_ptr<AppWindow>> result;

        auto lock = std::lock_guard<std::mutex>(m_appWindowLock);
        result.reserve(m_appWindows.size());
        for (auto& weakWindow : m_appWindows)
        {
            if (auto strong = weakWindow.lock())
            {
                result.emplace_back(std::move(strong));
            }
        }
        return result;
    }

    template <typename Lambda>
    static winrt::fire_and_forget BroadcastAsync(Lambda fn)
    {
        auto windows = GetAppWindows();
        for (const auto& windowRef : windows)
        {
            co_await windowRef.get()->DispatcherQueue();
            fn(*windowRef.get());
        }
    }

    // TODO: encapsulate this state in a class
    inline static reference_waiter m_appThreadsWaiter;
    inline static std::mutex m_appWindowLock;
    inline static std::vector<std::weak_ptr<AppWindow>> m_appWindows;

    bool m_rightClickLaunch{};
    wil::unique_hwnd m_window;
    HWND m_xamlSourceWindow{}; // This is owned by m_xamlSource, destroyed when Close() is called.

    std::shared_ptr<AppWindow> m_selfRef; // needed to extend lifetime durring async rundown
    std::optional<reference_waiter::reference_waiter_holder> m_appRefHolder; // need to ensure lifetime of the app process

    // This is needed to coordinate the use of Xaml from multiple threads.
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager m_xamlManager{nullptr};
    winrt::Windows::System::DispatcherQueueController m_queueController{ nullptr };
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource m_xamlSource{ nullptr };
    winrt::Windows::UI::Xaml::Controls::TextBlock m_status{ nullptr };
    winrt::Windows::UI::Xaml::UIElement::PointerPressed_revoker m_pointerPressedRevoker;
    winrt::Windows::UI::Xaml::XamlRoot::Changed_revoker m_rootChangedRevoker;
};

_Use_decl_annotations_ int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow)
{
    auto coInit = wil::CoInitializeEx();

    AppWindow::StartThreadAsync([nCmdShow, procRef = AppWindow::m_appThreadsWaiter.take_reference()](const auto&& queueController)
    {
        std::make_shared<AppWindow>(std::move(queueController))->Show(nCmdShow);
    });

    AppWindow::m_appThreadsWaiter.wait_until_zero();
    return 0;
}
