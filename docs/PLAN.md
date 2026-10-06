# Plan inicial de KRK Wakeup

Fecha de investigación: 2026-10-06. Estado: propuesta para conversar antes de implementar.

## Objetivo

Crear una utilidad nativa de Windows 10 y posteriores que envíe periódicamente una señal breve a la salida elegida para los parlantes KRK. Debe seguir utilizando esa salida aunque Windows cambie su salida predeterminada a unos auriculares Bluetooth.

El primer alcance confirmado es un par de GoAux 3 conectado por RCA y Windows 11 de 64 bits. Queda por identificar la salida del PC o interfaz que alimenta el cable RCA. Windows 10 permanece como objetivo posterior de compatibilidad.

Preferencias confirmadas: icono junto al reloj con menú, inicio con Windows opcional, funcionamiento continuo mientras la utilidad esté activa y aceptación de una señal apenas perceptible si es necesaria.

## Hallazgos y límites de la investigación

- El [manual oficial GoAux 3/4](https://cdn.shopify.com/s/files/1/0566/0809/6342/files/KRK-GoAux-Monitor-System-Product-User-Manual.pdf?v=1705178896), enlazado desde KRK, documenta el control de reposo: dos pulsaciones para salir y una pulsación de tres segundos para entrar. No especifica en la sección de controles el tiempo de reposo automático ni el umbral de audio que lo reinicia.
- Las búsquedas devolvieron referencias a 30 minutos, pero no se encontró confirmación primaria de ese valor para GoAux. No se adoptará como especificación ni como base para un intervalo fijo. Tampoco se trasladarán valores o funciones de ROKIT a GoAux.
- Las [entradas de GoAux 3](https://www.krkmusic.com/products/goaux-3-portable-powered-studio-monitors) incluyen Bluetooth, RCA y auxiliar. [GoAux 4](https://www.krkmusic.com/products/goaux-4-portable-powered-studio-monitors) también ofrece USB y TRS balanceado.
- El manual describe que conectar auriculares al conector frontal de los GoAux silencia los parlantes. Esa conexión es distinta de unos auriculares Bluetooth independientes conectados a Windows.
- Microsoft documenta que se puede abrir una salida concreta mediante su identificador con [IMMDeviceEnumerator::GetDevice](https://learn.microsoft.com/en-us/windows/win32/coreaudio/getting-the-default-device-endpoint-for-stream-routing). Esa será la base del direccionamiento de audio.
- [IMMDevice::GetId](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdevice-getid) permite guardar el identificador y recuperar la salida después. Se trata como un valor opaco; no se analiza su contenido ni se asume que nunca cambie tras reinstalaciones o cambios de hardware.

El repositorio contiene documentación; aún no hay mediciones ni pruebas en Windows o GoAux.

## Comportamiento propuesto

1. Primera apertura: elegir explícitamente la salida y ejecutar una prueba manual antes de activar los pulsos periódicos.
2. Mostrar un icono junto al reloj. Menú: activar/pausar, probar señal, configuración y salir.
3. Mostrar estados precisos: activo, pausado, dispositivo desconectado o error. Un envío correcto no demuestra que el amplificador permanezca despierto.
4. Recordar la salida seleccionada por identificador. Cambiar el dispositivo predeterminado no debe cambiar el destino de los pulsos.
5. Si desaparece esa salida, detener los envíos y esperar su regreso. Nunca elegir automáticamente auriculares u otra salida. Si cambia el identificador y no puede reconocerse con certeza, solicitar una nueva selección.
6. Ofrecer ajustes de intervalo, duración y nivel; los valores iniciales dependerán de la validación física. Incluir inicio con Windows opcional y funcionamiento continuo mientras esté activa.
7. Respetar el volumen y mute existentes. Informar cuando el mute o volumen impida una prueba; no subir automáticamente el volumen del sistema.
8. Permitir la suspensión normal del equipo. Mientras el equipo duerma o esté apagado no habrá pulsos. Al reanudar, recuperar la salida y el temporizador sin acumular envíos atrasados.
9. Trabajar en modo de audio compartido para convivir con otras aplicaciones. Si una aplicación ocupa la salida de forma exclusiva, informar y reintentar sin interrumpirla.

## Qué significa «solo a estos parlantes»

Con USB o Bluetooth, el usuario puede seleccionar la salida correspondiente a los KRK si está disponible en Windows. La disponibilidad simultánea de varios dispositivos Bluetooth debe comprobarse en el equipo real.

Con una conexión analógica, el destino seleccionable es la salida de la tarjeta o interfaz. Windows no puede garantizar qué parlantes físicos están conectados al extremo del cable. Una salida compartida, duplicada o redirigida por el controlador necesita comprobarse antes de prometer exclusividad.

## Señal: validar antes de fijar valores

La señal debe ser breve, de nivel bajo y con entrada/salida suaves para evitar clics. Frecuencia, nivel y duración se calibrarán juntos en la conexión real. Un archivo de silencio o mantener abierto el dispositivo no se considerará eficaz sin una prueba de reposo.

No hay evidencia suficiente para prometer una señal completamente inaudible que evite el reposo. Tampoco se presupone que un tono fuera del rango audible atraviese la cadena de audio o sea detectado por el circuito de reposo. El usuario acepta un sonido apenas perceptible si es necesario; se buscará el mínimo nivel eficaz comprobado.

Procedimiento propuesto:

1. Medir al menos dos veces el tiempo de reposo sin la utilidad y anotar modelo, conexión y volúmenes.
2. Probar señales breves a bajo nivel, con ajustes manuales y límites de amplitud. Como rango experimental de duración, evaluar aproximadamente 0,5–2 segundos; no es una especificación confirmada de GoAux.
3. Comprobar que cada señal candidata reinicie efectivamente el tiempo de reposo y evaluar su audibilidad en silencio.
4. Elegir un intervalo con margen respecto al menor tiempo medido; como criterio inicial de diseño, no más de un tercio de ese tiempo. No fijar 25 minutos basándose en una supuesta espera de 30.
5. Validar durante varias ventanas completas de reposo y repetir con auriculares Bluetooth como salida predeterminada.
6. Probar por separado la recuperación desde reposo. Prevenir el reposo no garantiza despertar un dispositivo que ya se durmió o se desconectó.

## Arquitectura recomendada, pendiente de cerrar

Una aplicación C++ con Win32 para la interfaz y [WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi) para el audio. El [icono de notificación](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw) puede implementarse con Shell_NotifyIcon. Esta propuesta prioriza un ejecutable pequeño y dependencias mínimas.

Componentes previstos:

- Interfaz: icono, menú y ventana pequeña de configuración.
- Dispositivos: enumeración, selección guardada y notificaciones de conexión/desconexión.
- Audio: generador de señal y reproducción compartida sobre la salida seleccionada.
- Planificador: temporizador, pausa y recuperación tras suspensión.
- Preferencias y diagnóstico: configuración por usuario y registro local acotado de envíos/errores.

La primera distribución propuesta es un ejecutable portable para Windows 11 x64, que es el sistema del usuario. El alcance acordado se limita a 64 bits; x64 se propone como arquitectura inicial, sin asumir cobertura ARM64. Windows 10 se validará después, definiendo las versiones concretas que se soportarán. CMake y compilación automatizada en Windows se plantean para la implementación posterior.

## Entregas propuestas

1. **Validación técnica:** una herramienta mínima para seleccionar la salida y probar/calibrar la señal en los KRK del usuario. Comenzar después de cerrar las preguntas de alcance.
2. **MVP:** icono, configuración, activación/pausa, temporizador y persistencia del dispositivo.
3. **Robustez:** desconexión, reconexión, cambios de salida predeterminada, suspensión, mute y salida ocupada.
4. **Distribución:** paquete portable, instrucciones y compilación automatizada. Antes de hacerlo público, elegir licencia y documentar las combinaciones de modelo/conexión realmente verificadas.

## Criterios de aceptación

- Mantener los GoAux despiertos durante varias ventanas de reposo, con el perfil de señal documentado y aceptado por el usuario.
- Cambiar a auriculares Bluetooth como salida predeterminada sin enviarles la señal de esta aplicación.
- Desconectar los KRK sin que el pulso se redirija a otra salida.
- Recuperar la misma salida al reconectar y al reanudar Windows, o mostrar claramente que se requiere intervención.
- Convivir con reproducción normal y manejar de forma visible una salida ocupada en modo exclusivo.
- Pausar y salir deben detener los envíos. Verificar volumen bajo, mute e inicio opcional con Windows.
- Probar primero en Windows 11 x64; verificar Windows 10 en una fase posterior antes de anunciar soporte probado. La compilación o los tests automáticos no sustituyen las pruebas físicas de reposo.

## Preguntas pendientes

1. ¿El cable RCA viene de un conector de 3,5 mm del PC, de un monitor o de una interfaz/DAC USB? ¿Cómo aparece esa salida en Windows?
2. Medir con el usuario el tiempo real de reposo y validar la señal en los GoAux 3; la evidencia actual no permite fijar esos valores.

Las decisiones de licencia, idiomas e instalador pueden cerrarse después de verificar la viabilidad del audio. El repositorio permanecerá privado hasta que el usuario solicite cambiar su visibilidad.
