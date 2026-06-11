#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "error_info.hpp"
#include "logger_config.hpp"

namespace fsp
{
  // ---------------------------------------------------------------------------
  // Thread-local ime niti — vsaka nit nastavi svoje ime, ki ga formatter bere.
  // Definirano kot inline, da je vidno v vseh prevajalnih enotah brez ODR kršitev.
  // ---------------------------------------------------------------------------
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, cert-err58-cpp)
  inline thread_local std::string log_thread_name = "unknown";

  // ---------------------------------------------------------------------------
  // Custom spdlog flag formatter: izpisuje log_thread_name trenutne niti.
  // Registriraj ga kot '%*' v vzorcu formatiranja.
  // ---------------------------------------------------------------------------
  class ThreadNameFormatter : public spdlog::custom_flag_formatter
  {
  public:
    void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*tm*/, spdlog::memory_buf_t& dest) override
    {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      dest.append(log_thread_name.data(), log_thread_name.data() + log_thread_name.size());
    }

    [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override { return std::make_unique<ThreadNameFormatter>(); }
  };

  // ---------------------------------------------------------------------------
  // fsp_logger — tanek razred okrog spdlog::logger.
  //
  // Odgovornosti:
  //   - gradi spdlog logger iz logger_config (konzola, datoteka ali oboje)
  //   - hrani shared_ptr<spdlog::logger> ki ga je mogoče deliti z workerji
  //     in validacijsko nitjo
  //   - ponuja typed log_*() metode s preverjanjem ravni (izogibamo se
  //     gradnji sporočilnih nizov ko raven ni aktivna)
  //   - izpostavlja underlying shared_ptr za kodo ki neposredno kliče spdlog
  //
  // Uporaba:
  //   fsp::fsp_logger lg(cfg.log_config);
  //   lg.info("Začenjam.");
  //   auto sptr = lg.get();          // za posredovanje workerjem
  // ---------------------------------------------------------------------------
  class fsp_logger
  {
  public:
    // Zgradi logger glede na logger_config.
    // Če sta enable_console in enable_file oba false, samodejno odpre barvno
    // konzolno izhajanje kot rezervni izhod.
    explicit fsp_logger(const logger_config& cfg) { build(cfg); }

    // Nekopirljiv, nepremakljiv — lastništvo se prenaša prek shared_ptr.
    fsp_logger(const fsp_logger&)            = delete;
    fsp_logger& operator=(const fsp_logger&) = delete;
    fsp_logger(fsp_logger&&)                 = delete;
    fsp_logger& operator=(fsp_logger&&)      = delete;

    ~fsp_logger()
    {
      if (logger_) logger_->flush();
    }

    // -----------------------------------------------------------------------
    // Dostop do underlying spdlog loggerja
    // -----------------------------------------------------------------------

    /// Vrne shared_ptr ki ga je varno kopirati in posredovati nitim.
    [[nodiscard]] std::shared_ptr<spdlog::logger> get() const { return logger_; }

    // -----------------------------------------------------------------------
    // Typed log metode — preverjanje ravni pred gradnjo sporočila.
    // [[unlikely]] ker večina klicev v kritični poti ne bo logirala.
    // -----------------------------------------------------------------------

    void critical(const error_info& e) const
    {
      if (active(spdlog::level::critical)) [[unlikely]]
        logger_->critical(e.to_string());
    }

    void error(const error_info& e) const
    {
      if (active(spdlog::level::err)) [[unlikely]]
        logger_->error(e.to_string());
    }

    void critical(std::string_view msg) const
    {
      if (active(spdlog::level::critical)) [[unlikely]]
        logger_->critical(msg);
    }
    void error(std::string_view msg) const
    {
      if (active(spdlog::level::err)) [[unlikely]]
        logger_->error(msg);
    }
    void warning(std::string_view msg) const
    {
      if (active(spdlog::level::warn)) [[unlikely]]
        logger_->warn(msg);
    }

    void info(std::string_view msg) const
    {
      if (active(spdlog::level::info)) [[unlikely]]
        logger_->info(msg);
    }

    void debug(std::string_view msg) const
    {
      if (active(spdlog::level::debug)) [[unlikely]]
        logger_->debug(msg);
    }

    void trace(std::string_view msg) const
    {
      if (active(spdlog::level::trace)) [[unlikely]]
        logger_->trace(msg);
    }

    // -----------------------------------------------------------------------
    // Pomožna metoda za pogojno logiranje brez shared_ptr dereferencing
    // -----------------------------------------------------------------------

    /// Vrne true če je logger živ in ima aktivno raven >= lvl.
    [[nodiscard]] bool active(spdlog::level::level_enum lvl = spdlog::level::trace) const noexcept
    { return logger_ && logger_->should_log(lvl); }
  private:
    std::shared_ptr<spdlog::logger> logger_;

    // Pomožna funkcija: zgradi spdlog::pattern_formatter z '%*' flagom za ime niti.
    static std::unique_ptr<spdlog::pattern_formatter> make_formatter(std::string_view pattern)
    {
      std::unordered_map<char, std::unique_ptr<spdlog::custom_flag_formatter>> flags;
      flags['*'] = std::make_unique<ThreadNameFormatter>();
      return std::make_unique<spdlog::pattern_formatter>(
        std::string(pattern), spdlog::pattern_time_type::local, spdlog::details::os::default_eol, std::move(flags));
    }

    void build(const logger_config& cfg)
    {
      log_thread_name = "main >";

      std::vector<spdlog::sink_ptr> sinks;

      if (cfg.enable_console)
      {
        auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v"));
        sinks.push_back(std::move(s));
      }

      if (cfg.enable_file && ! cfg.log_file_path.empty())
      {
        auto s = std::make_shared<spdlog::sinks::basic_file_sink_mt>(cfg.log_file_path, true);
        s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%-5l] %v"));
        sinks.push_back(std::move(s));
      }

      // Rezervna konzola če noben sink ni bil konfiguriran
      if (sinks.empty())
      {
        auto s = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s->set_formatter(make_formatter("[%Y-%m-%d %H:%M:%S.%e] [%*] [%^%-5l%$] %v"));
        sinks.push_back(std::move(s));
      }

      logger_ = std::make_shared<spdlog::logger>(cfg.logger_name, sinks.begin(), sinks.end());
      logger_->set_level(cfg.log_level);
      logger_->flush_on(spdlog::level::err);
      logger_->flush();
    }
  };

  // ---------------------------------------------------------------------------
  // Globalna pomožna funkcija — ohranjena za kompatibilnost z obstoječo kodo
  // ki preverja shared_ptr<spdlog::logger> neposredno (npr. v workerjih).
  // ---------------------------------------------------------------------------
  inline bool log_active(const std::shared_ptr<spdlog::logger>& lg, spdlog::level::level_enum lvl = spdlog::level::trace) noexcept
  { return lg && lg->should_log(lvl); }

} // namespace fsp
