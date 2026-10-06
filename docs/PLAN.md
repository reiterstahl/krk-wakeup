# Plan inicial de KRK Wakeup

Fecha de investigación: 2026-10-06. Estado: primer prototipo implementado; eficacia del pulso pendiente de pruebas en GoAux 3.

## Objetivo

Crear una utilidad nativa de Windows 10 y posteriores que envíe periódicamente una señal breve a la salida elegida para los parlantes KRK. Debe seguir utilizando esa salida aunque Windows cambie su salida predeterminada a unos auriculares Bluetooth.

El primer alcance confirmado es Windows 11 de 64 bits con esta conexión permanente: PC → USB → SMSL SU-1 → RCA → KRK GoAux 3. El destino de la aplicación será la salida de reproducción de Windows correspondiente al SMSL SU-1, seleccionada por el usuario y guardada por identificador. El nombre exacto mostrado por el controlador se comprobará durante la primera configuración. Windows 10 permanece como objetivo posterior de compatibilidad.

Preferencias confirmadas: icono junto al reloj con menú, inicio con Windows opcional, funcionamiento continuo mientras la utilidad esté activa y aceptación de una señal apenas perceptible si es necesaria.

## Hallazgos y límites de la investigación

- El [manual oficial GoAux 3/4](https://cdn.shopify.com/s/files/1/0566/0809/6342/files/KRK-GoAux-Monitor-System-Product-User-Manual.pdf?v=1705178896), enlazado desde KRK, documenta el control de reposo: dos pulsaciones para salir y una pulsación de tres segundos para entrar. No especifica en la sección de controles el tiempo de reposo automático ni el umbral de audio que lo reinicia.
- Las búsquedas devolvieron referencias a 30 minutos, pero no se encontró confirmación primaria de ese valor para GoAux. No se adoptará como especificación ni como base para un intervalo fijo. Tampoco se trasladarán valores o funciones de ROKIT a GoAux.
- Las [entradas de GoAux 3](https://www.krkmusic.com/products/goaux-3-portable-powered-studio-monitors) incluyen Bluetooth, RCA y auxiliar. [GoAux 4](https://www.krkmusic.com/products/goaux-4-portable-powered-studio-monitors) también ofrece USB y TRS balanceado.
- El manual describe que conectar auriculares al conector frontal de los GoAux silencia los parlantes. Esa conexión es distinta de unos auriculares Bluetooth independientes conectados a Windows.
- Microsoft documenta que se puede abrir una salida concreta mediante su identificador con [IMMDeviceEnumerator::GetDevice](https://learn.microsoft.com/en-us/windows/win32/coreaudio/getting-the-default-device-endpoint-for-stream-routing). Esa será la base del direccionamiento de audio.
- [IMMDevice::GetId](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdevice-getid) permite guardar el identificador y recuperar la salida después. Se trata como un valor opaco; no se analiza su contenido ni se asume que nunca cambie tras reinstalaciones o cambios de hardware.

El usuario estima un tiempo de reposo de aproximadamente 15 minutos en su configuración. Es una referencia inicial aportada por el usuario, pendiente de medición controlada; no una especificación del fabricante. El repositorio ya contiene una aplicación para Windows, compilación automática y una prueba de arranque. El comportamiento de reposo sigue pendiente de validación física en GoAux 3.

## Comportamiento propuesto

1. Primera apertura: elegir explícitamente la salida y ejecutar una prueba manual antes de activar los pulsos periódicos.
2. Mostrar un icono junto al reloj. Menú: activar/pausar, probar señal, configuración y salir.
3. Mostrar estados precisos: activo, pausado, dispositivo desconectado o error. Un envío correcto no demuestra que el amplificador permanezca despierto.
4. Recordar la salida seleccionada por identificador. Cambiar el dispositivo predeterminado no debe cambiar el destino de los pulsos.
5. Si desaparece esa salida, detener los envíos y esperar su regreso. Nunca elegir automáticamente auriculares u otra salida. Si cambia el identificador y no puede reconocerse con certeza, solicitar una nueva selección.
6. Ofrecer ajustes de intervalo, duración y nivel. Para las primeras pruebas se propone un pulso cada 5 minutos, con una referencia editable de reposo estimado de 15 minutos. Los parámetros de señal se validarán físicamente. Incluir inicio con Windows opcional y funcionamiento continuo mientras esté activa.
7. Respetar el volumen y mute existentes. Informar cuando el mute o volumen impida una prueba; no subir automáticamente el volumen del sistema.
8. Permitir la suspensión normal del equipo. Mientras el equipo duerma o esté apagado no habrá pulsos. Al reanudar, recuperar la salida y el temporizador sin acumular envíos atrasados.
9. Trabajar en modo de audio compartido para convivir con otras aplicaciones. Si una aplicación ocupa la salida de forma exclusiva, informar y reintentar sin interrumpirla.

## Qué significa «solo a estos parlantes»

En la configuración confirmada, la aplicación enviará los pulsos a la salida USB del SMSL SU-1. Los auriculares Bluetooth pueden ser la salida predeterminada de Windows sin cambiar ese destino. El usuario seleccionará el DAC una vez; no se dependerá de que la salida contenga «KRK» en su nombre.

Windows identifica el DAC, no los parlantes conectados por RCA. La aplicación podrá detectar la desaparición de la salida USB, pero no debe dar por detectable que se retire el cable RCA o se apaguen los GoAux mientras el DAC sigue conectado. Una salida compartida, duplicada o redirigida por el controlador necesita comprobarse antes de prometer exclusividad.

## Campos de tiempo para las pruebas

| Campo | Valor inicial | Función |
| --- | --- | --- |
| Reposo estimado de los parlantes | 15 minutos, editable | Referencia para comparar con el intervalo de envío; no modifica el temporizador interno de los GoAux. |
| Intervalo entre pulsos | 5 minutos, editable | Tiempo entre inicios de pulsos mientras la utilidad está activa. |
| Duración de la señal | Ajustable, pendiente de calibración | Tiempo durante el cual se reproduce cada pulso. |

Permitir introducir minutos y segundos. Guardar los valores por usuario y mostrar el tiempo hasta el siguiente pulso. Los cambios de intervalo reinician la cuenta desde su aplicación, sin generar una ráfaga de pulsos pendientes. El botón «Probar señal» envía un único pulso y, si el envío termina correctamente mientras la utilidad está activa, reinicia la cuenta del siguiente pulso.

Validar valores positivos, límites explícitos y que la duración sea menor que el intervalo. Si el intervalo iguala o supera el reposo estimado, mostrar un aviso de que puede no evitarlo; mantener la posibilidad de experimentar. Editar la referencia de reposo no cambiará silenciosamente el intervalo elegido. Los 5 minutos son una propuesta de partida con margen respecto a los 15 estimados, pendiente de verificación física.

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

## Arquitectura implementada

La aplicación usa C++ con Win32 para la interfaz y [WASAPI](https://learn.microsoft.com/en-us/windows/win32/coreaudio/wasapi) para el audio. El [icono de notificación](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw) usa Shell_NotifyIcon. La compilación enlaza el runtime de C++ estáticamente para facilitar un ejecutable portable.

Componentes previstos:

- Interfaz: icono, menú y ventana pequeña de configuración.
- Dispositivos: enumeración, selección guardada y notificaciones de conexión/desconexión.
- Audio: generador de señal y reproducción compartida sobre la salida seleccionada.
- Planificador: temporizador, pausa y recuperación tras suspensión.
- Preferencias y diagnóstico: configuración por usuario y registro local acotado de envíos/errores.

La primera distribución es un ejecutable portable para Windows 11 x64. El alcance acordado se limita a 64 bits; x64 se propone como arquitectura inicial, sin asumir cobertura ARM64. Windows 10 se validará después, definiendo las versiones concretas que se soportarán. La compilación usa CMake y GitHub Actions en Windows.

## Entregas propuestas

1. **Implementado:** selección de salida, pulso de prueba, icono, configuración, activar/pausar, temporizador y persistencia del dispositivo.
2. **Implementado:** espera y reintento cuando la salida no está disponible, sin usar otra salida; compilación automatizada y ejecutable portable.
3. **Pendiente de prueba física:** calibrar la señal y demostrar que evita el reposo, que permanece en el SU-1 al cambiar la salida predeterminada y que se recupera después de desconexión o suspensión.
4. **Antes de anunciar soporte adicional o publicar una versión con licencia abierta:** verificar Windows 10, elegir licencia y documentar las combinaciones de modelo/conexión realmente probadas.

## Criterios de aceptación

- Mantener los GoAux despiertos durante varias ventanas de reposo, con el perfil de señal documentado y aceptado por el usuario.
- Cambiar a auriculares Bluetooth como salida predeterminada sin enviarles la señal de esta aplicación.
- Desconectar el USB del SMSL SU-1 sin que el pulso se redirija a otra salida. Retirar solo los RCA o apagar los parlantes no debe presentarse como un evento que la aplicación necesariamente pueda detectar.
- Recuperar la misma salida al reconectar y al reanudar Windows, o mostrar claramente que se requiere intervención.
- Convivir con reproducción normal y manejar de forma visible una salida ocupada en modo exclusivo.
- Pausar y salir deben detener los envíos. Verificar volumen bajo, mute e inicio opcional con Windows.
- Probar primero en Windows 11 x64; verificar Windows 10 en una fase posterior antes de anunciar soporte probado. La compilación o los tests automáticos no sustituyen las pruebas físicas de reposo.

## Validaciones pendientes

1. Seleccionar en Windows la salida correspondiente al SMSL SU-1 y comprobar manualmente que alimenta los GoAux 3.
2. Medir el tiempo real de reposo tomando 15 minutos como referencia inicial y validar la señal con un intervalo inicial propuesto de 5 minutos. Ajustar ambos campos durante las pruebas.
3. Confirmar en los parlantes el nivel, frecuencia y duración mínimos eficaces; aún no hay un perfil de señal validado.

Las decisiones de licencia, idiomas e instalador pueden cerrarse después de verificar la viabilidad del audio. El repositorio es público como prototipo, todavía sin licencia de código abierto.
