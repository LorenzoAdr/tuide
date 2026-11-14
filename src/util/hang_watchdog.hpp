#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

// Vigía de cuelgues de la UI, pensado para ir siempre encendido.
//
// El modo de trazas (monitor_log) apunta todo por si acaso y lo paga en cada tecla.
// Aquí el camino caliente no escribe nada: un hilo aparte deja cada segundo una
// tarea vacía en la cola del bucle de FTXUI (las closures no invalidan el frame,
// así que no hay repintado) y solo si no se ejecuta a tiempo vuelca, una vez, el
// backtrace de todos los hilos a ~/.config/tuide/hangs/. Al no depender de marcas
// dentro del código, detecta igual un deadlock, un bucle infinito, un write()
// bloqueado contra el terminal o un cuelgue antes de entrar en Loop.
namespace tuide::hang_watchdog {

struct Options {
  std::chrono::milliseconds threshold{3000};
  std::chrono::milliseconds probe_interval{1000};
  // Segundo volcado si el cuelgue sigue: permite distinguir un bloqueo (pilas
  // idénticas) de un bucle (pilas que cambian).
  std::chrono::milliseconds second_dump_after{15000};
  // Antes del primer render (arranque, carga de workspace, docker exec…) el margen
  // es mayor: ahí se tolera más que en mitad de la edición.
  std::chrono::milliseconds startup_threshold{10000};
  // Vacío: $HOME/.config/tuide/hangs.
  std::string directory;
  int keep_reports = 10;
};

// Encola una tarea en el hilo de UI. Debe ser seguro llamarla desde otro hilo.
using UiPoster = std::function<void(std::function<void()>)>;

// Guarda el termios actual. Llamar antes de que FTXUI ponga el terminal en raw,
// para que la salida de emergencia pueda devolverlo a su estado.
void save_terminal_state();

// Arranca el vigía. El hilo que llama queda registrado como hilo de UI.
// Instala SIGUSR1 (volcado bajo demanda) y SIGUSR2 (volcado, restaurar terminal y
// salir): `pkill -USR2 ui-main` saca de un cuelgue sin reiniciar el contenedor.
void start(const Options& options, UiPoster poster);
void stop();
bool running();

// El bucle de FTXUI ya atiende tareas. Llamar desde el primer render: hasta entonces
// Post() descarta lo enviado y el vigía mide el arranque por reloj.
void mark_loop_running();

// Migas baratas que acompañan al volcado. Solo literales: el vigía lee el puntero
// desde otro hilo cuando la fase ya puede haber terminado.
void set_phase(const char* literal);
const char* phase();
void note_key_action(int action, const char* id_literal_or_null);

class PhaseScope {
 public:
  explicit PhaseScope(const char* literal) : previous_(phase()) { set_phase(literal); }
  ~PhaseScope() { set_phase(previous_); }
  PhaseScope(const PhaseScope&) = delete;
  PhaseScope& operator=(const PhaseScope&) = delete;

 private:
  const char* previous_;
};

std::string reports_directory();

// Fija la ruta del informe de crash (directorio por defecto) aunque el vigía esté
// apagado. Llamar al arrancar, fuera de cualquier manejador de señal.
void prepare_crash_report();

// Async-signal-safe: para el crash handler.
void restore_terminal();
void write_crash_report(const char* reason);

}  // namespace tuide::hang_watchdog

#define TUIDE_HANG_PHASE_CONCAT_IMPL(a, b) a##b
#define TUIDE_HANG_PHASE_CONCAT(a, b) TUIDE_HANG_PHASE_CONCAT_IMPL(a, b)
#define TUIDE_HANG_PHASE(literal) \
  ::tuide::hang_watchdog::PhaseScope TUIDE_HANG_PHASE_CONCAT(_tuide_hang_phase_, __LINE__)(literal)
