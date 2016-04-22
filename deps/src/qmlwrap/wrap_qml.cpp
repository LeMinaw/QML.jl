#include <QGuiApplication>
#include <QLibraryInfo>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QtQml>

#include "julia_api.hpp"
#include "julia_display.hpp"
#include "julia_object.hpp"
#include "julia_signals.hpp"
#include "type_conversion.hpp"

namespace qmlwrap
{

void set_context_property(QQmlContext* ctx, const QString& name, jl_value_t* v)
{
  if(ctx == nullptr)
  {
    qWarning() << "Can't set property " << name << " on null context";
    return;
  }

  if(jl_type_morespecific(jl_typeof(v), (jl_value_t*)cxx_wrap::julia_type<QObject>()))
  {
    ctx->setContextProperty(name, cxx_wrap::convert_to_cpp<QObject*>(v));
    return;
  }

  QVariant qt_var = cxx_wrap::convert_to_cpp<QVariant>(v);
  if(!qt_var.isNull())
  {
    ctx->setContextProperty(name, qt_var);
    return;
  }

  if(jl_is_structtype(jl_typeof(v)))
  {
    ctx->setContextProperty(name, new qmlwrap::JuliaObject(v, ctx));
    return;
  }
}

/// Manage creation and destruction of the application and the QML engine,
struct ApplicationManager
{

  // Singleton implementation
  static ApplicationManager& instance()
  {
    static ApplicationManager app_manager;
    return app_manager;
  }

  ~ApplicationManager()
  {
    if(m_app != nullptr)
    {
      m_app->quit();
    }
    std::cout << "destroying app manager" << std::endl;
    cleanup();
  }

  // Initialize the QGuiApplication instance
  void init_application()
  {
    if(m_quit_called)
    {
      cleanup();
    }
    static int argc = 1;
    static std::vector<char*> argv_buffer;
    if(argv_buffer.empty())
    {
      argv_buffer.push_back(const_cast<char*>("JuliaQML"));
    }

    QGuiApplication* app = new QGuiApplication(argc, &argv_buffer[0]);
    QObject::connect(app, &QGuiApplication::aboutToQuit, [this]()
    {
      std::cout << "about to quit" << std::endl;
      m_quit_called = true;
      if(m_timer != nullptr)
      {
        uv_timer_stop(m_timer);
        uv_close((uv_handle_t*)m_timer, ApplicationManager::handle_quit);
      }
    });
  }

  // Init the app with a new QQmlApplicationEngine
  QQmlApplicationEngine* init_qmlapplicationengine()
  {
    check_no_engine();
    QQmlApplicationEngine* e = new QQmlApplicationEngine();
    set_engine(e);
    return e;
  }

  QQmlEngine* init_qmlengine()
  {
    check_no_engine();
    set_engine(new QQmlEngine());
    return m_engine;
  }

  QQuickView* init_qquickview()
  {
    check_no_engine();
    QQuickView* view = new QQuickView();
    set_engine(view->engine());
    return view;
  }

  void add_context_properties(cxx_wrap::ArrayRef<jl_value_t*> property_names, cxx_wrap::ArrayRef<jl_value_t*> properties)
  {
    if(property_names.size() != properties.size())
    {
      throw std::runtime_error("Property names and properties arrays sizes dont't match");
    }
    const std::size_t nb_props = properties.size();
    for(std::size_t i = 0; i != nb_props; ++i)
    {
      set_context_property(m_root_ctx, cxx_wrap::convert_to_cpp<QString>(property_names[i]), properties[i]);
    }
  }

  QQmlContext* root_context()
  {
    return m_root_ctx;
  }

  // Blocking call to exec, running the Qt event loop
  void exec()
  {
    m_running = true;
    QGuiApplication::exec();
    std::cout << "exec returned" << std::endl;
    cleanup();
  }

  // Non-blocking exec, polling for Qt events in the uv event loop using a uv_timer_t
  void exec_async()
  {
    m_timer = new uv_timer_t();
    uv_timer_init(jl_global_event_loop(), m_timer);
    uv_timer_start(m_timer, ApplicationManager::process_events, 15, 15);
  }

private:

  ApplicationManager()
  {
  }

  void cleanup()
  {
    std::cout << "doing cleanup" << std::endl;
    JuliaAPI::instance()->on_about_to_quit();
    delete m_engine;
    delete m_app;
    delete m_timer;
    m_engine = nullptr;
    m_app = nullptr;
    m_timer = nullptr;
    m_running = false;
    m_quit_called = false;
  }

  void check_no_engine()
  {
    if(m_quit_called)
    {
      cleanup();
    }
    if(m_engine != nullptr)
    {
      throw std::runtime_error("Existing engine, aborting creation");
    }
    if(m_app == nullptr)
    {
      init_application();
    }
  }

  void set_engine(QQmlEngine* e)
  {
    m_engine = e;
    m_root_ctx = e->rootContext();
  }

  static void process_events(uv_timer_t*)
  {
    QGuiApplication::sendPostedEvents();
    QGuiApplication::processEvents();
  }

  static void handle_quit(uv_handle_t* handle)
  {
    uv_unref(handle);
    ApplicationManager::instance().cleanup();
  }

  QGuiApplication* m_app = nullptr;
  QQmlEngine* m_engine = nullptr;
  QQmlContext* m_root_ctx = nullptr;
  uv_timer_t* m_timer = nullptr;
  bool m_running = false;
  bool m_quit_called = false;
};

void load_qml_app(const QString& path, cxx_wrap::ArrayRef<jl_value_t*> property_names, cxx_wrap::ArrayRef<jl_value_t*> context_properties)
{
  auto e = ApplicationManager::instance().init_qmlapplicationengine();
  ApplicationManager::instance().add_context_properties(property_names, context_properties);
  e->load(path);
}


} // namespace qmlwrap

JULIA_CPP_MODULE_BEGIN(registry)
  using namespace cxx_wrap;

  Module& qml_module = registry.create_module("QML");

  qmlRegisterSingletonType("org.julialang", 1, 0, "Julia", qmlwrap::julia_js_singletontype_provider);
  qmlRegisterType<qmlwrap::JuliaSignals>("org.julialang", 1, 0, "JuliaSignals");
  qmlRegisterType<qmlwrap::JuliaDisplay>("org.julialang", 1, 0, "JuliaDisplay");

  qml_module.add_abstract<QObject>("QObject");

  qml_module.add_type<QQmlContext>("QQmlContext", julia_type<QObject>())
    .method("context_property", &QQmlContext::contextProperty);
  qml_module.method("set_context_property", qmlwrap::set_context_property);

  qml_module.add_type<QQmlEngine>("QQmlEngine", julia_type<QObject>())
    .method("root_context", &QQmlEngine::rootContext);

  qml_module.add_type<QQmlApplicationEngine>("QQmlApplicationEngine", julia_type<QQmlEngine>())
    .constructor<QString>() // Construct with path to QML
    .method("load", static_cast<void (QQmlApplicationEngine::*)(const QString&)>(&QQmlApplicationEngine::load)); // cast needed because load is overloaded

  qml_module.method("qt_prefix_path", []() { return QLibraryInfo::location(QLibraryInfo::PrefixPath); });

  qml_module.add_abstract<QQuickItem>("QQuickItem");

  qml_module.add_abstract<QQuickWindow>("QQuickWindow")
    .method("content_item", &QQuickWindow::contentItem);

  qml_module.add_type<QQuickView>("QQuickView", julia_type<QQuickWindow>())
    .method("set_source", &QQuickView::setSource)
    .method("show", &QQuickView::show) // not exported: conflicts with Base.show
    .method("engine", &QQuickView::engine)
    .method("root_object", &QQuickView::rootObject);

  qml_module.add_type<QByteArray>("QByteArray").constructor<const char*>();
  qml_module.add_type<QQmlComponent>("QQmlComponent", julia_type<QObject>())
    .constructor<QQmlEngine*>()
    .method("set_data", &QQmlComponent::setData);
  qml_module.method("create", [](QQmlComponent& comp, QQmlContext* context)
  {
    if(!comp.isReady())
    {
      qWarning() << "QQmlComponent is not ready, aborting create";
      return;
    }

    QObject* obj = comp.create(context);
    if(context != nullptr)
    {
      obj->setParent(context); // setting this makes sure the new object gets deleted
    }
  });

  // App manager functions
  qml_module.method("init_application", []() { qmlwrap::ApplicationManager::instance().init_application(); });
  qml_module.method("init_qmlapplicationengine", []() { return qmlwrap::ApplicationManager::instance().init_qmlapplicationengine(); });
  qml_module.method("init_qmlengine", []() { return qmlwrap::ApplicationManager::instance().init_qmlengine(); });
  qml_module.method("init_qquickview", []() { return qmlwrap::ApplicationManager::instance().init_qquickview(); });
  qml_module.method("qmlcontext", []() { return qmlwrap::ApplicationManager::instance().root_context(); });
  qml_module.method("load_qml_app", qmlwrap::load_qml_app);
  qml_module.method("exec", []() { qmlwrap::ApplicationManager::instance().exec(); });


  qml_module.add_type<QTimer>("QTimer", julia_type<QObject>());

  qml_module.add_type<qmlwrap::JuliaObject>("JuliaObject", julia_type<QObject>())
    .method("set", &qmlwrap::JuliaObject::set) // Not exported, use @qmlset
    .method("value", &qmlwrap::JuliaObject::value); // Not exported, use @qmlget

  // Emit signals helper
  qml_module.method("emit", [](const char* signal_name, cxx_wrap::ArrayRef<jl_value_t*> args)
  {
    using namespace qmlwrap;
    JuliaSignals* julia_signals = JuliaAPI::instance()->juliaSignals();
    if(julia_signals == nullptr)
    {
      throw std::runtime_error("No signals available");
    }
    julia_signals->emit_signal(signal_name, args);
  });

  // Function to register a function
  qml_module.method("register_function", [](cxx_wrap::ArrayRef<jl_value_t*> args)
  {
    for(jl_value_t* arg : args)
    {
      qmlwrap::JuliaAPI::instance()->register_function(convert_to_cpp<QString>(arg));
    }
  });

  qml_module.add_type<qmlwrap::JuliaDisplay>("JuliaDisplay", julia_type("CppDisplay"))
    .method("load_png", &qmlwrap::JuliaDisplay::load_png);

  // Exports:
  qml_module.export_symbols("QQmlContext", "set_context_property", "root_context", "load", "qt_prefix_path", "set_source", "engine", "QByteArray", "QQmlComponent", "set_data", "create", "QQuickItem", "content_item", "JuliaObject", "QTimer", "context_property", "emit", "JuliaDisplay", "init_application", "qmlcontext", "init_qmlapplicationengine", "init_qmlengine", "init_qquickview");
JULIA_CPP_MODULE_END