#include <cassert>

#include "components/builder.hpp"
#include "components/config.hpp"
#include "components/logger.hpp"
#include "events/signal.hpp"
#include "events/signal_emitter.hpp"
#include "modules/meta/base.hpp"

POLYBAR_NS

namespace modules {

  /**
   * Maps action names to function pointers in this module and invokes them.
   *
   * Each module has one instance of this class and uses it to register action.
   * For each action the module has to register the name, whether it can take
   * additional data, and a pointer to the member function implementing that
   * action.
   *
   * Ref: https://isocpp.org/wiki/faq/pointers-to-members
   *
   * The input() function in the base class uses this for invoking the actions
   * of that module.
   *
   * Any module that does not reimplement that function will automatically use
   * this class for action routing.
   */
  template <typename Impl>
  class action_router {
    typedef void (Impl::*callback)();
    typedef void (Impl::*callback_data)(const std::string&);

   public:
    explicit action_router(Impl* This) : m_this(This) {}

    void register_action(const string& name, callback func) {
      entry e;
      e.with_data = false;
      e.without = func;
      callbacks[name] = e;
    }

    void register_action_with_data(const string& name, callback_data func) {
      entry e;
      e.with_data = true;
      e.with = func;
      callbacks[name] = e;
    }

    bool has_action(const string& name) {
      return callbacks.find(name) != callbacks.end();
    }

    /**
     * Invokes the given action name on the passed module pointer.
     *
     * The action must exist.
     */
    void invoke(const string& name, const string& data) {
      auto it = callbacks.find(name);
      assert(it != callbacks.end());

      entry e = it->second;

      bool has_data = !data.empty();

      if (has_data && !e.with_data) {
        // TODO print error message
      }

#define CALL_MEMBER_FN(object, ptrToMember) ((object).*(ptrToMember))
      if (e.with_data) {
        CALL_MEMBER_FN(*m_this, e.with)(data);
      } else {
        CALL_MEMBER_FN(*m_this, e.without)();
      }
#undef CALL_MEMBER_FN
    }

   private:
    struct entry {
      union {
        callback without;
        callback_data with;
      };
      bool with_data;
    };
    std::unordered_map<string, entry> callbacks;
    Impl* m_this;
  };

  // module<Impl> public {{{

  template <typename Impl>
  module<Impl>::module(const bar_settings bar, string name)
      : m_sig(signal_emitter::make())
      , m_bar(bar)
      , m_log(logger::make())
      , m_conf(config::make())
      , m_router(make_unique<action_router<Impl>>(CAST_MOD(Impl)))
      , m_name("module/" + name)
      , m_name_raw(name)
      , m_builder(make_unique<builder>(bar))
      , m_formatter(make_unique<module_formatter>(m_conf, m_name))
      , m_handle_events(m_conf.get(m_name, "handle-events", true)) {}

  template <typename Impl>
  module<Impl>::~module() noexcept {
    m_log.trace("%s: Deconstructing", name());

    for (auto&& thread_ : m_threads) {
      if (thread_.joinable()) {
        thread_.join();
      }
    }
    if (m_mainthread.joinable()) {
      m_mainthread.join();
    }
  }

  template <typename Impl>
  string module<Impl>::name() const {
    return m_name;
  }

  template <typename Impl>
  string module<Impl>::name_raw() const {
    return m_name_raw;
  }

  template <typename Impl>
  string module<Impl>::type() const {
    return string(Impl::TYPE);
  }

  template <typename Impl>
  bool module<Impl>::running() const {
    return static_cast<bool>(m_enabled);
  }

  template <typename Impl>
  void module<Impl>::stop() {
    if (!static_cast<bool>(m_enabled)) {
      return;
    }

    m_log.info("%s: Stopping", name());
    m_enabled = false;

    std::lock(m_buildlock, m_updatelock);
    std::lock_guard<std::mutex> guard_a(m_buildlock, std::adopt_lock);
    std::lock_guard<std::mutex> guard_b(m_updatelock, std::adopt_lock);
    {
      CAST_MOD(Impl)->wakeup();
      CAST_MOD(Impl)->teardown();

      m_sig.emit(signals::eventqueue::check_state{});
    }
  }

  template <typename Impl>
  void module<Impl>::halt(string error_message) {
    m_log.err("%s: %s", name(), error_message);
    m_log.notice("Stopping '%s'...", name());
    stop();
  }

  template <typename Impl>
  void module<Impl>::teardown() {}

  template <typename Impl>
  string module<Impl>::contents() {
    if (m_changed) {
      m_log.info("%s: Rebuilding cache", name());
      m_cache = CAST_MOD(Impl)->get_output();
      // Make sure builder is really empty
      m_builder->flush();
      if (!m_cache.empty()) {
        // Add a reset tag after the module
        m_builder->control(tags::controltag::R);
        m_cache += m_builder->flush();
      }
      m_changed = false;
    }
    return m_cache;
  }

  template <typename Impl>
  bool module<Impl>::input(const string& name, const string& data) {
    if (!m_router->has_action(name)) {
      return false;
    }

    m_router->invoke(name, data);
    return true;
  }

  // }}}
  // module<Impl> protected {{{

  template <typename Impl>
  void module<Impl>::broadcast() {
    m_changed = true;
    m_sig.emit(signals::eventqueue::notify_change{});
  }

  template <typename Impl>
  void module<Impl>::idle() {
    if (running()) {
      CAST_MOD(Impl)->sleep(25ms);
    }
  }

  template <typename Impl>
  void module<Impl>::sleep(chrono::duration<double> sleep_duration) {
    if (running()) {
      std::unique_lock<std::mutex> lck(m_sleeplock);
      m_sleephandler.wait_for(lck, sleep_duration);
    }
  }

  template <typename Impl>
  template <class Clock, class Duration>
  void module<Impl>::sleep_until(chrono::time_point<Clock, Duration> point) {
    if (running()) {
      std::unique_lock<std::mutex> lck(m_sleeplock);
      m_sleephandler.wait_until(lck, point);
    }
  }

  template <typename Impl>
  void module<Impl>::wakeup() {
    m_log.trace("%s: Release sleep lock", name());
    m_sleephandler.notify_all();
  }

  template <typename Impl>
  string module<Impl>::get_format() const {
    return DEFAULT_FORMAT;
  }

  template <typename Impl>
  string module<Impl>::get_output() {
    std::lock_guard<std::mutex> guard(m_buildlock);
    auto format_name = CONST_MOD(Impl).get_format();
    auto format = m_formatter->get(format_name);
    bool no_tag_built{true};
    bool fake_no_tag_built{false};
    bool tag_built{false};
    auto mingap = std::max(1_z, format->spacing);
    size_t start, end;
    string value{format->value};
    while ((start = value.find('<')) != string::npos && (end = value.find('>', start)) != string::npos) {
      if (start > 0) {
        if (no_tag_built) {
          // If no module tag has been built we do not want to add
          // whitespace defined between the format tags, but we do still
          // want to output other non-tag content
          auto trimmed = string_util::ltrim(value.substr(0, start), ' ');
          if (!trimmed.empty()) {
            fake_no_tag_built = false;
            m_builder->node(move(trimmed));
          }
        } else {
          m_builder->node(value.substr(0, start));
        }
        value.erase(0, start);
        end -= start;
        start = 0;
      }
      string tag{value.substr(start, end + 1)};
      if (tag.empty()) {
        continue;
      } else if (tag[0] == '<' && tag[tag.size() - 1] == '>') {
        if (!no_tag_built)
          m_builder->space(format->spacing);
        else if (fake_no_tag_built)
          no_tag_built = false;
        if (!(tag_built = CONST_MOD(Impl).build(m_builder.get(), tag)) && !no_tag_built)
          m_builder->remove_trailing_space(mingap);
        if (tag_built)
          no_tag_built = false;
      }
      value.erase(0, tag.size());
    }

    if (!value.empty()) {
      m_builder->append(value);
    }

    return format->decorate(&*m_builder, m_builder->flush());
  }

  // }}}
}  // namespace modules

POLYBAR_NS_END
