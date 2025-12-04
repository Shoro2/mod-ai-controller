/*
 * Entry Point für das Modul.
 * Hier sagen wir dem Core: "Hey, lade bitte unsere Hooks."
 */

#include "ScriptMgr.h"

// Forward declaration der Funktion, die wir gleich in der anderen Datei schreiben
void AddAIControllerScripts();

// Diese Funktion wird vom Server beim Start automatisch aufgerufen
void Addmod_ai_controllerScripts() {
    AddAIControllerScripts();
}