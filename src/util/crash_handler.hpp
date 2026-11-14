#pragma once

namespace tuide {

void install_crash_handlers();

// FTXUI, al entrar en Loop, cambia SIGSEGV/SIGILL/SIGFPE/SIGABRT por un manejador que
// solo anota la señal y retorna. Ante un fallo real eso reejecuta la instrucción que
// falló: el hilo gira al 100 % para siempre y, si es el de UI, la app queda colgada
// con el terminal en raw. Llamar ya dentro de Loop para recuperar los nuestros.
void reinstall_fault_handlers();
void print_current_backtrace(const char* reason);

}  // namespace tuide
