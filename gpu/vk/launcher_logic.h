// (ps5, lot 69) The launch screen's logic, with no Vulkan, ImGui or file in it (vk_launcher.cpp draws it and reads the facts;
// tests/test_launcher_logic.py compiles this header alone): its texts in English and French, the settings it offers the player
// and the values each may take on the console, the shader check's verdict from the facts read, whether the screen shows at
// all, its pages and their buttons (no countdown: the player chooses to start, lot 71).
//
// What it offers is a choice made with the player (notes-agents/menu-lancement.md 2.2, lot 69's decisions): no row that makes
// the picture smaller, blurrier or sharper, none of the memory, driver or development switches. The values are the in-game
// menu's own rows (vk_menu.cpp rows[], found by their key in hookshot_options.ini), stepped through the lists below only: the
// frame rate cap is 30 or the console's 60 (the TV runs at 60 Hz with V-Sync: nothing above 60 is shown), frame generation off
// or 2x, the FPS counter without its Dev level.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "console_prepare.h"   // (lot 82: what the Shaders page offers of the console's own preparation)

namespace mkwvk::launcher {

// ---- the texts: each in English and in French (Lang), by its id ----
enum Lang { EN = 0, FR = 1 };
enum Text : int {
    T_PLAY, T_CONTINUE, T_SETTINGS, T_SHADERS, T_QUIT,
    T_HELP_PLAY, T_HELP_CONTINUE, T_HELP_SETTINGS, T_HELP_SHADERS, T_HELP_QUIT, T_QUIT_AGAIN,
    T_COUNTDOWN, T_COUNTDOWN_STOPPED, T_COUNTDOWN_NONE, T_MENU_SHORTCUT,
    T_READY, T_TO_COMPLETE, T_PROBLEM, T_CHECKING,
    T_PROMPT_CONFIRM, T_PROMPT_BACK, T_PROMPT_PLAY_NOW, T_PROMPT_TABS, T_PROMPT_DEFAULT, T_PROMPT_CHANGE, T_PROMPT_SETTINGS,
    T_TAB_PICTURE, T_TAB_GAME, T_TAB_CONTROLLER, T_TAB_SOUND, T_TAB_LAUNCH,
    T_SAVED_EACH_CHANGE, T_HELD_BY_ENV, T_PLATFORM,
    // settings: their names
    T_S_FPS_CAP, T_S_FRAMEGEN, T_S_FPS_COUNTER, T_S_TEXT_SPEED, T_S_TEXT_SIZE, T_S_SKIP_OPENING, T_S_GAME_LANG,
    T_S_MENU_LANG, T_S_MENU_SIZE, T_S_RUMBLE, T_S_GYRO, T_S_GYRO_X, T_S_GYRO_Y, T_S_GYRO_INV_X, T_S_GYRO_INV_Y,
    T_S_CAM_INV_X, T_S_CAM_INV_Y, T_S_CAM_SPEED, T_S_DEAD_ZONE, T_S_LAYOUT, T_S_VOLUME, T_S_LAUNCH_MODE,
    // and their help
    T_H_FPS_CAP, T_H_FRAMEGEN, T_H_FPS_COUNTER, T_H_TEXT_SPEED, T_H_TEXT_SIZE, T_H_SKIP_OPENING, T_H_GAME_LANG,
    T_H_MENU_LANG, T_H_MENU_SIZE, T_H_RUMBLE, T_H_GYRO, T_H_GYRO_X, T_H_GYRO_Y, T_H_GYRO_INV, T_H_CAM_INV, T_H_CAM_SPEED,
    T_H_DEAD_ZONE, T_H_LAYOUT, T_H_VOLUME, T_H_LAUNCH_MODE,
    // values
    T_V_30, T_V_UNLOCKED, T_V_OFF_F, T_V_X2, T_V_OFF_M, T_V_FPS, T_V_FRAME_TIME, T_V_GRAPH, T_V_BREAKDOWN, T_V_NORMAL_F,
    T_V_INSTANT, T_V_ORIGINAL, T_V_NO, T_V_YES, T_V_ON, T_V_OFF, T_V_AUTO_LANG, T_V_FRENCH, T_V_ENGLISH, T_V_SMALL,
    T_V_MEDIUM, T_V_LARGE, T_V_RUMBLE_OFF, T_V_DEAD_DEFAULT, T_V_NINTENDO, T_V_PLAYSTATION, T_V_ALWAYS, T_V_WHEN_NEEDED,
    // the Shaders page
    T_SH_VERSION, T_SH_PROGRAMS, T_SH_NOTES, T_SH_STATES, T_SH_DRIVER, T_SH_DRIVER_CHECK, T_SH_SKIPPED,
    T_SH_VERSION_OK, T_SH_VERSION_OTHER, T_SH_NONE_YET, T_SH_PROGRAMS_N, T_SH_NOTES_N, T_SH_STATES_N, T_SH_NONE_F,
    T_SH_DRIVER_MB, T_SH_DRIVER_EMPTY, T_SH_CHECK_NOT_ASKED, T_SH_CHECK_RUNNING, T_SH_CHECK_RESULT, T_SH_CHECK_IMPOSSIBLE,
    T_SH_SKIPPED_N, T_SH_CHECKED_IN, T_SH_ASK_DRIVER, T_SH_NOTHING_BUILT,
    T_SH_WHY_PROGRAMS, T_SH_WHY_VERSION, T_SH_WHY_DRIVER, T_SH_WHY_NOTES, T_SH_WHY_COVERAGE, T_SH_WHY_PARTIAL,
    T_SH_WHY_OK, T_SH_PLAY_ANYWAY,
    // (lot 82) the console's own shader preparation (HOOKSHOT_PS5_PREPARE=1)
    T_TO_PREPARE, T_TO_RESUME, T_SH_PREP, T_SH_PREP_DONE, T_SH_PREP_NONE, T_SH_PREP_BEGUN, T_SH_PREP_NO_CENSUS,
    T_SH_PREP_BUTTON, T_SH_PREP_RESUME_BUTTON, T_SH_PREP_AGAIN_BUTTON, T_SH_PREP_WHY_NEEDED, T_SH_PREP_WHY_RESUME,
    T_SH_PREP_WHY_OPTIONAL, T_SH_PREP_WHY_DONE, T_SH_PREP_WHY_NO_CENSUS, T_SH_PREP_NOTE, T_HELP_SHADERS_PREP,
    T_PREPARING, T_PREP_READING, T_PREP_STEP_MESH, T_PREP_STEP_COMPUTE, T_PREP_STEP_PIPELINES, T_PREP_SETTLING, T_PREP_COUNT,
    T_PREP_LEFT, T_PREP_LEFT_UNKNOWN, T_PREP_MEMORY, T_PREP_HELP, T_PREP_LATER, T_PREP_STOPPING, T_PREP_DONE, T_PREP_STOPPED,
    T_PREP_LOW_MEMORY, T_PREP_STALL, T_PREP_NOTHING, T_PREP_RESTARTING, T_PREP_CANNOT_START,
    T_COUNT
};
struct TextEntry {
    Text id;
    const char* en;
    const char* fr;
};
// (French as the in-game menu's French table writes it: "vous". No ✕, ○ or △ in a text: the fonts may not have them, and the
// prompts draw the buttons themselves; the texts say "cross" and "circle")
inline constexpr TextEntry kTexts[] = {
    {T_PLAY, "PLAY", "JOUER"},
    {T_CONTINUE, "CONTINUE", "CONTINUER"},
    {T_SETTINGS, "SETTINGS", "RÉGLAGES"},
    {T_SHADERS, "SHADERS", "SHADERS"},
    {T_QUIT, "QUIT", "QUITTER"},
    {T_HELP_PLAY, "Starts the game at its title screen.", "Lance le jeu à son écran titre."},
    {T_HELP_CONTINUE, "Starts the game and loads the most recent save (for this launch only).",
     "Lance le jeu et charge la sauvegarde la plus récente (pour ce lancement seulement)."},
    {T_HELP_SETTINGS, "Frame rate, frame generation, text, controller, sound and this screen.",
     "Cadence, génération d'images, texte, manette, son et cet écran."},
    {T_HELP_SHADERS, "Whether the shader caches are ready for this version of the game.",
     "Si les caches de shaders sont prêts pour cette version du jeu."},
    {T_HELP_QUIT, "Closes the game and goes back to the console's home screen.",
     "Ferme le jeu et revient à l'écran d'accueil de la console."},
    {T_QUIT_AGAIN, "Confirm again to quit.", "Validez encore pour quitter."},
    {T_COUNTDOWN, "The game starts in %d s. Any button stops the countdown.",
     "Le jeu démarre dans %d s. Une touche arrête le compte à rebours."},
    {T_COUNTDOWN_STOPPED, "Countdown stopped: choose with the D-pad.", "Compte à rebours arrêté : choisissez avec la croix."},
    {T_COUNTDOWN_NONE, "The shaders need a look first.", "Les shaders demandent d'abord un coup d'œil."},
    // (lot 79: the home page says how to open the in-game menu, host/nn_hid.c's chord on the console; lot 80: the port's public
    // name, Grapnel)
    {T_MENU_SHORTCUT, "In the game: touchpad + R1 opens the Grapnel menu.", "En jeu : pavé tactile + R1 ouvre le menu Grapnel."},
    {T_READY, "Ready", "Prêts"},
    {T_TO_COMPLETE, "To complete", "À compléter"},
    {T_PROBLEM, "Problem", "Problème"},
    {T_CHECKING, "Checking...", "Vérification..."},
    {T_PROMPT_CONFIRM, "Confirm", "Valider"},
    {T_PROMPT_BACK, "Back", "Retour"},
    {T_PROMPT_PLAY_NOW, "Play now", "Jouer tout de suite"},
    {T_PROMPT_TABS, "Tabs", "Onglets"},
    {T_PROMPT_DEFAULT, "Default", "Valeur d'origine"},
    {T_PROMPT_CHANGE, "Change", "Changer"},
    {T_PROMPT_SETTINGS, "Settings", "Réglages"},
    {T_TAB_PICTURE, "Picture", "Image"},
    {T_TAB_GAME, "Game", "Jeu"},
    {T_TAB_CONTROLLER, "Controller", "Manette"},
    {T_TAB_SOUND, "Sound", "Son"},
    {T_TAB_LAUNCH, "Launch", "Lancement"},
    {T_SAVED_EACH_CHANGE, "Saved at each change.", "Enregistré à chaque changement."},
    {T_HELD_BY_ENV, "Set by hookshot_env.txt (%s) for this launch.", "Fixé par hookshot_env.txt (%s) pour ce lancement."},
    {T_PLATFORM, "PlayStation 5", "PlayStation 5"},
    {T_S_FPS_CAP, "Game frame rate", "Cadence du jeu"},
    {T_S_FRAMEGEN, "Frame generation", "Génération d'images"},
    {T_S_FPS_COUNTER, "FPS counter", "Compteur d'images"},
    {T_S_TEXT_SPEED, "Text speed", "Vitesse du texte"},
    {T_S_TEXT_SIZE, "Text size", "Taille du texte"},
    {T_S_SKIP_OPENING, "Skip the opening", "Passer l'ouverture"},
    {T_S_GAME_LANG, "Game language", "Langue du jeu"},
    {T_S_MENU_LANG, "Menu language", "Langue des menus"},
    {T_S_MENU_SIZE, "Menu size", "Taille des menus"},
    {T_S_RUMBLE, "Vibration", "Vibrations"},
    {T_S_GYRO, "Gyroscope", "Gyroscope"},
    {T_S_GYRO_X, "Gyroscope sensitivity X", "Sensibilité du gyroscope X"},
    {T_S_GYRO_Y, "Gyroscope sensitivity Y", "Sensibilité du gyroscope Y"},
    {T_S_GYRO_INV_X, "Invert the gyroscope X", "Inverser le gyroscope X"},
    {T_S_GYRO_INV_Y, "Invert the gyroscope Y", "Inverser le gyroscope Y"},
    {T_S_CAM_INV_X, "Invert the camera X", "Inverser la caméra X"},
    {T_S_CAM_INV_Y, "Invert the camera Y", "Inverser la caméra Y"},
    {T_S_CAM_SPEED, "Camera speed", "Vitesse de la caméra"},
    {T_S_DEAD_ZONE, "Stick dead zone", "Zone morte des sticks"},
    {T_S_LAYOUT, "Button layout", "Disposition des boutons"},
    {T_S_VOLUME, "Volume", "Volume"},
    {T_S_LAUNCH_MODE, "Launch screen", "Mode de lancement"},
    {T_H_FPS_CAP,
     "How many frames the game itself draws each second. Unlocked: the console's normal cap of 60; the TV runs at 60 Hz with "
     "V-Sync, so nothing above 60 is shown. 30: an even rhythm when the game cannot hold 60. Applies at once.",
     "Le nombre d'images que le jeu calcule chaque seconde. Débloquée : la limite normale de la console, 60 ; la TV est à "
     "60 Hz avec la synchronisation verticale, rien au-delà de 60 n'est donc affiché. 30 : un rythme plus égal quand le jeu "
     "n'arrive pas à 60. S'applique tout de suite."},
    {T_H_FRAMEGEN,
     "Makes one frame between two of the game's for smoother motion, at a little controller delay. Off: the game's own "
     "frames only.",
     "Fabrique une image entre deux images du jeu pour un mouvement plus fluide, avec un peu de retard à la manette. "
     "Désactivée : les images du jeu seules."},
    {T_H_FPS_COUNTER, "The counter in a corner of the screen while you play.",
     "Le compteur dans un coin de l'écran pendant le jeu."},
    {T_H_TEXT_SPEED, "How fast the letters of dialogue and other messages appear.",
     "La vitesse à laquelle les lettres des dialogues et des messages s'affichent."},
    {T_H_TEXT_SIZE, "How large the text of dialogue, Navi and signs is drawn.",
     "La taille du texte des dialogues, de Navi et des panneaux."},
    {T_H_SKIP_OPENING, "A new game skips the opening movie and Navi waking Link. Later cutscenes are left alone.",
     "Une nouvelle partie saute le film d'ouverture et le réveil de Link par Navi. Les scènes suivantes restent."},
    {T_H_GAME_LANG, "The game's own texts (language.txt). Applies to this launch.",
     "Les textes du jeu lui-même (language.txt). S'applique à ce lancement."},
    {T_H_MENU_LANG, "This screen and the in-game menu (touchpad and R1 together). Auto: the game's language.",
     "Cet écran et le menu en jeu (pavé tactile et R1 ensemble). Auto : la langue du jeu."},
    {T_H_MENU_SIZE, "How large the in-game menu and the FPS counter are drawn (this screen keeps its size).",
     "La taille du menu en jeu et du compteur d'images (cet écran garde la sienne)."},
    {T_H_RUMBLE, "How strongly the controller vibrates, in percent of the game's own.",
     "La force des vibrations de la manette, en pourcentage de celles du jeu."},
    {T_H_GYRO, "Aims and looks with the controller's gyroscope where the game uses motion.",
     "Vise et regarde avec le gyroscope de la manette là où le jeu utilise le mouvement."},
    {T_H_GYRO_X, "How far the aim moves left and right for a turn of the controller. 100% is the console's own.",
     "De combien la visée bouge à gauche et à droite quand la manette tourne. 100 % : comme la console d'origine."},
    {T_H_GYRO_Y, "How far the aim moves up and down for a tilt of the controller. 100% is the console's own.",
     "De combien la visée bouge en haut et en bas quand la manette s'incline. 100 % : comme la console d'origine."},
    {T_H_GYRO_INV, "Turns that way of the motion aim around.", "Inverse ce sens de la visée au gyroscope."},
    {T_H_CAM_INV, "Turns that way of the right stick's camera around.", "Inverse ce sens de la caméra au stick droit."},
    {T_H_CAM_SPEED, "How fast the right stick turns the camera, in percent of the game's own speed.",
     "La vitesse à laquelle le stick droit tourne la caméra, en pourcentage de celle du jeu."},
    {T_H_DEAD_ZONE, "How far a stick moves before the game sees it: lower for a precise stick, higher for one that drifts.",
     "La course d'un stick avant que le jeu la voie : plus bas pour un stick précis, plus haut pour un stick qui dérive."},
    {T_H_LAYOUT,
     "Which face button does what in the game, and which one confirms on this screen. Nintendo: circle confirms and is the "
     "game's A (action, talk), cross is B. PlayStation: cross confirms and is A, circle is B.",
     "Quel bouton fait quoi en jeu, et lequel valide sur cet écran. Nintendo : rond valide et sert de A (action, parler), "
     "croix de B. PlayStation : croix valide et sert de A, rond de B."},
    {T_H_VOLUME, "How loud the game plays.", "Le volume du jeu."},
    {T_H_LAUNCH_MODE,
     "Always: this screen at every launch, until you choose. When needed: the game starts directly unless the "
     "shader check finds a problem; hold R1 while the game starts to see this screen.",
     "Toujours : cet écran à chaque lancement, jusqu'à votre choix. Si nécessaire : le jeu démarre "
     "directement, sauf si la vérification des shaders trouve un problème ; tenez R1 au lancement pour voir cet écran."},
    {T_V_30, "30 fps", "30 images/s"},
    {T_V_UNLOCKED, "Unlocked (60)", "Débloquée (60)"},
    {T_V_OFF_F, "Off", "Désactivée"},
    {T_V_X2, "x2", "x2"},
    {T_V_OFF_M, "Off", "Désactivé"},
    {T_V_FPS, "FPS", "Images/s"},
    {T_V_FRAME_TIME, "Frame time", "Temps d'image"},
    {T_V_GRAPH, "Graph", "Graphique"},
    {T_V_BREAKDOWN, "Breakdown", "Détail"},
    {T_V_NORMAL_F, "Normal", "Normale"},
    {T_V_INSTANT, "Instant", "Instantanée"},
    {T_V_ORIGINAL, "Original", "Originale"},
    {T_V_NO, "No", "Non"},
    {T_V_YES, "Yes", "Oui"},
    {T_V_ON, "On", "Activé"},
    {T_V_OFF, "Off", "Désactivé"},
    {T_V_AUTO_LANG, "Auto (the game's)", "Auto (celle du jeu)"},
    {T_V_FRENCH, "Français", "Français"},
    {T_V_ENGLISH, "English", "English"},
    {T_V_SMALL, "Small", "Petite"},
    {T_V_MEDIUM, "Medium", "Moyenne"},
    {T_V_LARGE, "Large", "Grande"},
    {T_V_RUMBLE_OFF, "Off", "Désactivées"},
    {T_V_DEAD_DEFAULT, "24% (default)", "24 % (d'origine)"},
    {T_V_NINTENDO, "Nintendo", "Nintendo"},
    {T_V_PLAYSTATION, "PlayStation", "PlayStation"},
    {T_V_ALWAYS, "Always", "Toujours"},
    {T_V_WHEN_NEEDED, "When needed", "Si nécessaire"},
    {T_SH_VERSION, "Shader version", "Version des shaders"},
    {T_SH_PROGRAMS, "Translated programs", "Programmes traduits"},
    {T_SH_NOTES, "Pipelines seen in play", "Pipelines vues en jeu"},
    {T_SH_STATES, "Pipeline list", "Liste des pipelines"},
    {T_SH_DRIVER, "Driver cache", "Cache du pilote"},
    {T_SH_DRIVER_CHECK, "The driver asked", "Question au pilote"},
    {T_SH_SKIPPED, "Programs left out", "Programmes écartés"},
    {T_SH_VERSION_OK, "%s", "%s"},
    {T_SH_VERSION_OTHER, "%s (another)", "%s (autre)"},
    {T_SH_NONE_YET, "none yet", "aucun pour l'instant"},
    {T_SH_PROGRAMS_N, "%s (%s MB)", "%s (%s Mo)"},
    {T_SH_NOTES_N, "%s (%d%% translated)", "%s (%d %% traduits)"},
    {T_SH_STATES_N, "%s", "%s"},
    {T_SH_NONE_F, "none", "aucune"},
    {T_SH_DRIVER_MB, "%s MB", "%s Mo"},
    {T_SH_DRIVER_EMPTY, "empty or missing", "vide ou absent"},
    {T_SH_CHECK_NOT_ASKED, "not asked", "pas demandé"},
    {T_SH_CHECK_RUNNING, "asking...", "en cours..."},
    {T_SH_CHECK_RESULT, "%d of %d found", "%d sur %d trouvées"},
    {T_SH_CHECK_IMPOSSIBLE, "not possible here", "impossible ici"},
    {T_SH_SKIPPED_N, "%s", "%s"},
    {T_SH_CHECKED_IN, "Checked in %.1f s, by reading the files. The driver is asked without a compile, about pipelines play "
                      "noted; the pipeline list rebuilds the caches after an update.",
     "Vérifié en %.1f s, en lisant les fichiers. Le pilote est interrogé sans compilation, sur des pipelines notées en "
     "jeu ; la liste des pipelines sert à refaire les caches après une mise à jour."},
    {T_SH_ASK_DRIVER, "Ask the driver (64 pipelines)", "Vérifier auprès du pilote (64 pipelines)"},
    {T_SH_NOTHING_BUILT, "Nothing is built on the console in this version: the caches are prepared on the PC.",
     "Rien n'est construit sur la console dans cette version : les caches sont préparés sur le PC."},
    {T_SH_WHY_PROGRAMS,
     "The program cache is missing: effects will be translated and compiled while you play (short stops the first time "
     "each is seen). You can still play.",
     "Le cache de programmes manque : les effets seront traduits et compilés en jouant (arrêts brefs la première fois que "
     "chacun apparaît). Vous pouvez quand même jouer."},
    {T_SH_WHY_VERSION,
     "The program cache was made for another version of the shaders: it will be made again while you play (short stops "
     "the first time each effect is seen). You can still play.",
     "Le cache de programmes vient d'une autre version des shaders : il sera refait en jouant (arrêts brefs la première "
     "fois que chaque effet apparaît). Vous pouvez quand même jouer."},
    {T_SH_WHY_DRIVER,
     "The driver's cache is empty or missing: effects will be compiled while you play (short stops the first time each is "
     "seen). You can still play.",
     "Le cache du pilote est vide ou absent : les effets seront compilés en jouant (arrêts brefs la première fois que "
     "chacun apparaît). Vous pouvez quand même jouer."},
    {T_SH_WHY_NOTES, "No notes of the pipelines seen in play for this version: the list fills as you play.",
     "Pas de notes des pipelines vues en jeu pour cette version : la liste se remplit en jouant."},
    {T_SH_WHY_COVERAGE, "Some pipelines seen in play have no translated program: those will be translated when first seen.",
     "Des pipelines vues en jeu n'ont pas leur programme traduit : ils le seront à leur première vue."},
    {T_SH_WHY_PARTIAL, "The driver's cache does not hold every pipeline asked: some effects will be compiled while you play.",
     "Le cache du pilote n'a pas toutes les pipelines demandées : des effets seront compilés en jouant."},
    {T_SH_WHY_OK, "The caches are ready for this version.", "Les caches sont prêts pour cette version."},
    {T_SH_PLAY_ANYWAY, "PLAY stays available.", "JOUER reste possible."},
    // (lot 82) the console's own shader preparation
    {T_TO_PREPARE, "To prepare", "À préparer"},
    {T_TO_RESUME, "To resume", "À reprendre"},
    {T_SH_PREP, "Preparation on the console", "Préparation sur la console"},
    {T_SH_PREP_DONE, "done", "faite"},
    {T_SH_PREP_NONE, "not done", "pas faite"},
    {T_SH_PREP_BEGUN, "%d%% done", "faite à %d %%"},
    {T_SH_PREP_NO_CENSUS, "not possible", "impossible"},
    {T_SH_PREP_BUTTON, "Prepare the shaders (about %s)", "Préparer les shaders (environ %s)"},
    {T_SH_PREP_RESUME_BUTTON, "Resume the preparation (about %s left)", "Reprendre la préparation (environ %s)"},
    {T_SH_PREP_AGAIN_BUTTON, "Prepare anyway (about %s)", "Préparer quand même (environ %s)"},
    {T_SH_PREP_WHY_NEEDED,
     "The console can compile the game's shaders once now, so that play does not stop to compile them. You can stop it at any "
     "time: it goes on where it stopped.",
     "La console peut compiler une fois pour toutes les shaders du jeu, pour que le jeu ne s'arrête pas pour les compiler en "
     "jouant. Vous pouvez l'arrêter à tout moment : elle reprendra là où elle s'est arrêtée."},
    {T_SH_PREP_WHY_RESUME, "A preparation was begun: it goes on where it stopped.",
     "Une préparation a été commencée : elle reprend là où elle s'est arrêtée."},
    {T_SH_PREP_WHY_OPTIONAL, "The caches look made already: a preparation would only add what they miss.",
     "Les caches semblent déjà faits : une préparation n'ajouterait que ce qui leur manque."},
    {T_SH_PREP_WHY_DONE, "The shaders were prepared on this console for this version.",
     "Les shaders ont été préparés sur cette console pour cette version."},
    {T_SH_PREP_WHY_NO_CENSUS,
     "No preparation possible: the list of the game's shaders, which the builder makes from your game, is not beside the game.",
     "Pas de préparation possible : la liste des shaders du jeu, que le constructeur fait depuis votre jeu, n'est pas à côté du "
     "jeu."},
    {T_SH_PREP_NOTE, "The console prepares its driver's cache itself, from your game's shaders.",
     "La console prépare elle-même le cache de son pilote, depuis les shaders de votre jeu."},
    {T_HELP_SHADERS_PREP, "The shaders can be prepared once on the console: play then does not stop to compile them.",
     "Les shaders peuvent être préparés une fois sur la console : le jeu ne s'arrête plus pour les compiler."},
    {T_PREPARING, "PREPARATION", "PRÉPARATION"},
    {T_PREP_READING, "Reading the game's shaders...", "Lecture des shaders du jeu..."},
    {T_PREP_STEP_MESH, "Step 1 of 3: mesh programs", "Étape 1 sur 3 : programmes mesh"},
    {T_PREP_STEP_COMPUTE, "Step 2 of 3: compute programs", "Étape 2 sur 3 : programmes de calcul"},
    {T_PREP_STEP_PIPELINES, "Step 3 of 3: pipelines", "Étape 3 sur 3 : pipelines"},
    {T_PREP_SETTLING, "Writing the driver's cache...", "Écriture du cache du pilote..."},
    {T_PREP_COUNT, "%s of %s", "%s sur %s"},
    {T_PREP_LEFT, "Time left: about %s", "Temps restant : environ %s"},
    {T_PREP_LEFT_UNKNOWN, "Time left: being measured...", "Temps restant : en cours d'estimation..."},
    {T_PREP_MEMORY, "Free memory: %s MB", "Mémoire libre : %s Mo"},
    {T_PREP_HELP,
     "The console compiles the game's shaders once, for its driver. \"Later\" stops it: what is made is kept, and the next "
     "preparation goes on from there.",
     "La console compile une fois pour toutes les shaders du jeu pour son pilote. « Plus tard » l'arrête : ce qui est fait "
     "est gardé, et la prochaine préparation reprend de là."},
    {T_PREP_LATER, "Later", "Plus tard"},
    {T_PREP_STOPPING, "Stopping: the compiles under way finish first...", "Arrêt : les compilations en cours se terminent..."},
    {T_PREP_DONE, "Done. The game starts again, to give its memory back.", "Terminé. Le jeu redémarre, pour rendre sa mémoire."},
    {T_PREP_STOPPED, "Stopped. The game starts again, to give its memory back; the preparation will go on from here.",
     "Arrêtée. Le jeu redémarre, pour rendre sa mémoire ; la préparation reprendra d'ici."},
    {T_PREP_LOW_MEMORY, "Memory is getting short: the game starts again and goes on with the preparation.",
     "La mémoire se fait rare : le jeu redémarre et reprend la préparation."},
    {T_PREP_STALL, "A compile does not end: the game starts again and goes on without it.",
     "Une compilation ne se termine pas : le jeu redémarre et reprend sans elle."},
    {T_PREP_NOTHING, "The preparation could not read the game's shaders: nothing was made.",
     "La préparation n'a pas pu lire les shaders du jeu : rien n'a été fait."},
    {T_PREP_RESTARTING, "Starting again...", "Redémarrage..."},
    {T_PREP_CANNOT_START, "The preparation needs a fresh start of the game: it starts again and begins.",
     "La préparation demande un nouveau démarrage du jeu : il redémarre et commence."},
};
static_assert(sizeof kTexts / sizeof kTexts[0] == T_COUNT, "a text for each id, in order");
inline const char* text(Text id, Lang l) {
    if (id < 0 || id >= T_COUNT) return "";
    return l == FR ? kTexts[id].fr : kTexts[id].en;
}

// ---- the settings offered ----
enum Tab { TAB_PICTURE, TAB_GAME, TAB_CONTROLLER, TAB_SOUND, TAB_LAUNCH, TAB_N };
inline constexpr Text kTabNames[TAB_N] = {T_TAB_PICTURE, T_TAB_GAME, T_TAB_CONTROLLER, T_TAB_SOUND, T_TAB_LAUNCH};
/// How a setting's value is named (value_text)
enum Names {
    N_FPS_CAP, N_FRAMEGEN, N_FPS_COUNTER, N_TEXT_SPEED, N_TEXT_SIZE, N_NO_YES, N_GAME_LANG, N_MENU_LANG, N_MENU_SIZE,
    N_RUMBLE, N_ON_OFF, N_PERCENT, N_DEAD_ZONE, N_LAYOUT, N_LAUNCH_MODE
};
/// The launch screen's own setting, not a row of the in-game menu: launcher= in hookshot_options.ini (vk_menu.cpp keeps it)
inline constexpr const char* kLaunchModeKey = "launcher";
constexpr int kMaxValues = 16;
struct Setting {
    Tab tab;
    const char* key;   // (its row's key in hookshot_options.ini: vk_menu.cpp ini_key, the control bridge's menu_setting_set)
    Text name, help;
    Names names;
    int lo, hi, step;            // (a range: lo to hi by step, stopping at the ends; or, n_values > 0, a list)
    int values[kMaxValues];      // (the list, in the order the arrows go, wrapping round)
    int n_values;
    int fallback;                // (the value the Default button puts back; -9999: none)
    const char* env[3];          // (the development switches that hold it for the run, read over the row: greyed then)
};
constexpr int kNoDefault = -9999;
// (the menu's Auto, Français and English: vk_menu.cpp opts.menu_lang, 0 Auto then menu_i18n.h kLangs' place plus one: en is
// the first, fr the fifth. tests/test_launcher_logic.py checks the places against menu_i18n.h)
constexpr int kMenuLangAuto = 0, kMenuLangEnglish = 1, kMenuLangFrench = 5;
inline constexpr Setting kSettings[] = {
    // Picture
    {TAB_PICTURE, "frame_rate_cap", T_S_FPS_CAP, T_H_FPS_CAP, N_FPS_CAP, 0, 0, 0, {-1, 0}, 2, 0, {"HOS_FPS_MAX"}},
    {TAB_PICTURE, "frame_generation", T_S_FRAMEGEN, T_H_FRAMEGEN, N_FRAMEGEN, 0, 0, 0, {0, 2}, 2, 2,
     {"RX_VK_FRAMEGEN_MULT", "RX_VK_FRAMEGEN"}},
    {TAB_PICTURE, "fps_counter", T_S_FPS_COUNTER, T_H_FPS_COUNTER, N_FPS_COUNTER, 0, 0, 0, {0, 1, 2, 3, 4}, 5, 1, {}},
    // Game
    {TAB_GAME, "text_speed", T_S_TEXT_SPEED, T_H_TEXT_SPEED, N_TEXT_SPEED, 0, 0, 0, {0, 1, 2, 3}, 4, 0, {"HOS_TEXT_SPEED"}},
    {TAB_GAME, "text_size", T_S_TEXT_SIZE, T_H_TEXT_SIZE, N_TEXT_SIZE, 0, 0, 0, {0, 1, 2, 3, 4}, 5, 0, {"HOS_TEXT_SIZE"}},
    {TAB_GAME, "skip_opening", T_S_SKIP_OPENING, T_H_SKIP_OPENING, N_NO_YES, 0, 0, 0, {0, 1}, 2, 0, {"HOS_SKIP_OPENING"}},
    {TAB_GAME, "game_language", T_S_GAME_LANG, T_H_GAME_LANG, N_GAME_LANG, 0, 15, 1, {}, 0, kNoDefault, {"HOS_LANGUAGE"}},
    {TAB_GAME, "menu_language", T_S_MENU_LANG, T_H_MENU_LANG, N_MENU_LANG, 0, 0, 0,
     {kMenuLangAuto, kMenuLangFrench, kMenuLangEnglish}, 3, kMenuLangAuto, {}},
    {TAB_GAME, "menu_size", T_S_MENU_SIZE, T_H_MENU_SIZE, N_MENU_SIZE, 0, 0, 0, {1, 2, 3}, 3, 2, {}},
    // Controller
    {TAB_CONTROLLER, "layout_preset", T_S_LAYOUT, T_H_LAYOUT, N_LAYOUT, 0, 0, 0, {0, 1}, 2, 0, {}},
    {TAB_CONTROLLER, "rumble_strength", T_S_RUMBLE, T_H_RUMBLE, N_RUMBLE, 0, 150, 25, {}, 0, 100, {}},
    {TAB_CONTROLLER, "motion_controls", T_S_GYRO, T_H_GYRO, N_ON_OFF, 0, 0, 0, {1, 0}, 2, 1, {}},
    {TAB_CONTROLLER, "motion_sensitivity_x", T_S_GYRO_X, T_H_GYRO_X, N_PERCENT, 10, 600, 10, {}, 0, 100, {}},
    {TAB_CONTROLLER, "motion_sensitivity_y", T_S_GYRO_Y, T_H_GYRO_Y, N_PERCENT, 10, 300, 10, {}, 0, 100, {}},
    {TAB_CONTROLLER, "invert_motion_x", T_S_GYRO_INV_X, T_H_GYRO_INV, N_NO_YES, 0, 0, 0, {0, 1}, 2, 0, {}},
    {TAB_CONTROLLER, "invert_motion_y", T_S_GYRO_INV_Y, T_H_GYRO_INV, N_NO_YES, 0, 0, 0, {0, 1}, 2, 0, {}},
    {TAB_CONTROLLER, "invert_camera_x", T_S_CAM_INV_X, T_H_CAM_INV, N_NO_YES, 0, 0, 0, {0, 1}, 2, 0, {}},
    {TAB_CONTROLLER, "invert_camera_y", T_S_CAM_INV_Y, T_H_CAM_INV, N_NO_YES, 0, 0, 0, {0, 1}, 2, 0, {}},
    {TAB_CONTROLLER, "camera_speed", T_S_CAM_SPEED, T_H_CAM_SPEED, N_PERCENT, 50, 400, 25, {}, 0, 100, {}},
    {TAB_CONTROLLER, "stick_dead_zone", T_S_DEAD_ZONE, T_H_DEAD_ZONE, N_DEAD_ZONE, 0, 0, 0, {0, 1, 2, 3, 4, 5}, 6, 0, {}},
    // Sound
    {TAB_SOUND, "volume", T_S_VOLUME, T_H_VOLUME, N_PERCENT, 0, 100, 5, {}, 0, 100, {}},
    // Launch
    {TAB_LAUNCH, kLaunchModeKey, T_S_LAUNCH_MODE, T_H_LAUNCH_MODE, N_LAUNCH_MODE, 0, 0, 0, {0, 1}, 2, 0, {}},
};
constexpr int kSettingCount = (int)(sizeof kSettings / sizeof kSettings[0]);
/// The keys no setting may ever have (the picture, the memory, the driver: notes-agents/menu-lancement.md 2.4): checked
/// against kSettings by tests/test_launcher_logic.py
inline constexpr const char* kNeverOffered[] = {
    "field_of_view", "damage_taken", "crt_filter", "sharpness", "resolution", "quality_preset", "upscaling", "game_speed",
    "hdr", "output_res", "handheld_profile", "ultrawide", "dlss", "frame_pacing", "anisotropic_filtering", "shadow_resolution",
    "robust_buffers", "gpu_device", "vsync", "threaded_rendering", "descriptor_reuse", "sampler_cache", "astc_as_rgba8",
};

/// The settings of a tab, in order: how many, and the index in kSettings of its i-th
inline int tab_count(int tab) {
    int n = 0;
    for (const Setting& s : kSettings) n += s.tab == tab;
    return n;
}
inline int tab_setting(int tab, int i) {
    for (int k = 0, n = 0; k < kSettingCount; k++)
        if (kSettings[k].tab == tab && n++ == i) return k;
    return -1;
}
inline int setting_of(const char* key) {
    for (int k = 0; k < kSettingCount; k++)
        if (!strcmp(kSettings[k].key, key)) return k;
    return -1;
}
/// The value after v by dir (+1 or -1): the list's next, round from its end to its start (a value not in the list goes to its
/// first, or its last going back); a range's next step, stopping at its ends (a value off the steps: onto them)
inline int next_value(const Setting& s, int v, int dir) {
    if (s.n_values > 0) {
        int at = -1;
        for (int i = 0; i < s.n_values; i++)
            if (s.values[i] == v) at = i;
        if (at < 0) return dir > 0 ? s.values[0] : s.values[s.n_values - 1];
        return s.values[(at + (dir > 0 ? 1 : s.n_values - 1)) % s.n_values];
    }
    if (s.step <= 0) return v;
    int n = v + (dir > 0 ? s.step : -s.step);
    if ((v - s.lo) % s.step) n = s.lo + ((v - s.lo) / s.step + (dir > 0 ? 1 : 0)) * s.step;   // (onto the steps)
    return n < s.lo ? s.lo : n > s.hi ? s.hi : n;
}
/// Whether v is a value the screen offers for s (the in-game menu's file may hold another: shown as it is, the arrows go to
/// the list from it)
inline bool offered(const Setting& s, int v) {
    if (s.n_values > 0) {
        for (int i = 0; i < s.n_values; i++)
            if (s.values[i] == v) return true;
        return false;
    }
    return v >= s.lo && v <= s.hi && (s.step <= 0 || (v - s.lo) % s.step == 0);
}
/// The development switch that holds s for this run (getenv given as a function, for the tests), or nullptr
template <class Getenv>
const char* held_by(const Setting& s, Getenv get) {
    for (const char* e : s.env)
        if (e && get(e)) {
            // (RX_VK_FRAMEGEN=0 turns frame generation off; another value leaves the row alone)
            if (!strcmp(e, "RX_VK_FRAMEGEN") && atoi(get(e)) != 0) continue;
            return e;
        }
    return nullptr;
}

// (the game's languages: vk_menu_i18n.cpp kGameLangs' order, named in the screen's two languages, so that no letter the
// fonts lack is drawn: tests/test_launcher_logic.py checks the order against vk_menu_i18n.cpp)
inline constexpr const char* kGameLangCodes[16] = {"en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419",
                                                   "pt-BR", "nl", "pl", "ru", "ja", "ko", "zh-Hans", "zh-Hant"};
inline constexpr const char* kGameLangEn[16] = {
    "English (US)", "English (UK)", "French (France)", "French (Canada)", "German", "Italian", "Spanish (Spain)",
    "Spanish (Latin America)", "Portuguese (Brazil)", "Dutch", "Polish", "Russian", "Japanese", "Korean",
    "Chinese (simplified)", "Chinese (traditional)"};
inline constexpr const char* kGameLangFr[16] = {
    "Anglais (États-Unis)", "Anglais (Royaume-Uni)", "Français (France)", "Français (Canada)", "Allemand", "Italien",
    "Espagnol (Espagne)", "Espagnol (Amérique latine)", "Portugais (Brésil)", "Néerlandais", "Polonais", "Russe", "Japonais",
    "Coréen", "Chinois (simplifié)", "Chinois (traditionnel)"};

/// A setting's value as the screen names it, into buf
inline const char* value_text(const Setting& s, int v, Lang l, char* buf, size_t cap) {
    const auto t = [&](Text id) { return text(id, l); };
    const auto percent = [&](int p) {
        snprintf(buf, cap, l == FR ? "%d %%" : "%d%%", p);
        return (const char*)buf;
    };
    switch (s.names) {
    case N_FPS_CAP:
        if (v == -1) return t(T_V_30);
        if (v == 0) return t(T_V_UNLOCKED);
        {   // (a cap above 60 from the PC's menu: the console's TV shows 60 at most)
            static const int caps[] = {60, 90, 120, 144, 165, 240};
            if (v > 0 && v < 6) snprintf(buf, cap, "%d", caps[v]);
            else snprintf(buf, cap, "%s", l == FR ? "Sans limite" : "Uncapped");
            return buf;
        }
    case N_FRAMEGEN:
        if (v == 0) return t(T_V_OFF_F);
        if (v == 2) return t(T_V_X2);
        if (v == 1) return "Auto";
        snprintf(buf, cap, "x%d", v);
        return buf;
    case N_FPS_COUNTER: {
        static const Text n[] = {T_V_OFF_M, T_V_FPS, T_V_FRAME_TIME, T_V_GRAPH, T_V_BREAKDOWN};
        if (v >= 0 && v < 5) return t(n[v]);
        return "Dev";
    }
    case N_TEXT_SPEED:
        if (v == 1) return "x2";
        if (v == 2) return "x4";
        return t(v == 3 ? T_V_INSTANT : T_V_NORMAL_F);
    case N_TEXT_SIZE: {
        static const int p[] = {100, 110, 125, 150, 175};
        if (v <= 0 || v > 4) return t(T_V_ORIGINAL);
        return percent(p[v]);
    }
    case N_NO_YES: return t(v ? T_V_YES : T_V_NO);
    case N_ON_OFF: return t(v ? T_V_ON : T_V_OFF);
    case N_GAME_LANG: return v >= 0 && v < 16 ? (l == FR ? kGameLangFr[v] : kGameLangEn[v]) : "?";
    case N_MENU_LANG:
        if (v == kMenuLangAuto) return t(T_V_AUTO_LANG);
        if (v == kMenuLangFrench) return t(T_V_FRENCH);
        if (v == kMenuLangEnglish) return t(T_V_ENGLISH);
        return l == FR ? "Autre (menu en jeu)" : "Other (in-game menu)";
    case N_MENU_SIZE: return t(v == 1 ? T_V_SMALL : v == 3 ? T_V_LARGE : T_V_MEDIUM);
    case N_RUMBLE: return v <= 0 ? t(T_V_RUMBLE_OFF) : percent(v);
    case N_PERCENT: return percent(v);
    case N_DEAD_ZONE: {
        static const int p[] = {24, 5, 10, 15, 20, 30};
        if (v <= 0 || v > 5) return t(T_V_DEAD_DEFAULT);
        return percent(p[v]);
    }
    case N_LAYOUT:
        if (v == 0) return t(T_V_NINTENDO);
        if (v == 1) return t(T_V_PLAYSTATION);
        return l == FR ? "Personnalisée (menu en jeu)" : "Custom (in-game menu)";
    case N_LAUNCH_MODE: return t(v ? T_V_WHEN_NEEDED : T_V_ALWAYS);
    }
    return "";
}
/// The language the screen speaks: French for the menu's French, or for its Auto when the game's language is French
/// (fr, fr-CA); English otherwise (the screen has those two)
inline Lang lang_for(int menu_lang, const char* game_code) {
    if (menu_lang == kMenuLangFrench) return FR;
    if (menu_lang == kMenuLangAuto) return game_code && (game_code[0] | 0x20) == 'f' && (game_code[1] | 0x20) == 'r' ? FR : EN;
    return EN;
}

// ---- the shader check: what the files say (launcher_check.h reads them), and the verdict ----
enum Verdict { V_OK, V_WARN, V_FAIL };
struct ShaderFacts {
    bool programs_present = false;   // (hookshot_programs.bin)
    bool programs_magic = false;     // (its head: "HSP1" or "HSP2")
    uint64_t programs_stamp = 0;     // (the stamp in its head)
    uint64_t programs_bytes = 0;
    bool index_present = false, index_ok = false;   // (hookshot_programs.idx: "HPIX", its version, the same stamp)
    size_t programs = 0;             // (the index's records)
    bool notes_present = false, notes_ok = false;   // (hookshot_pipelines.bin: the stamp and the note size)
    size_t notes = 0, notes_covered = 0;            // (notes, and those whose program the index has)
    bool states_present = false, states_ok = false; // (hookshot_pipeline_states.bin: "HKSTATE2" or "HKSTATE1")
    size_t states = 0;
    bool driver_known = false;       // (a driver cache folder was looked at)
    uint64_t driver_bytes = 0;
    size_t skipped = 0;              // (skip.txt's programs)
    double seconds = 0;
    bool prepared = false;           // (lot 82: the console's own preparation finished for this translator and driver)
};
/// Each line's state on the Shaders page, and the whole verdict (the worst): FAIL when play will translate or compile the
/// effects anyway (no program cache, or one of another version, or no driver cache), WARN when part of it is missing
enum Why { WHY_OK, WHY_PROGRAMS, WHY_VERSION, WHY_DRIVER, WHY_NOTES, WHY_COVERAGE };
struct Check {
    Verdict verdict = V_OK;
    Why why = WHY_OK;              // (the first reason, the one the home page says)
    Verdict version = V_OK, programs = V_OK, notes = V_OK, driver = V_OK;
    int coverage = 100;          // (percent of the notes whose program is translated)
};
inline Check judge(const ShaderFacts& f, uint64_t stamp) {
    Check c;
    const auto worse = [&](Verdict v, Why w) {
        if (v > c.verdict) c.verdict = v, c.why = w;
        else if (v == c.verdict && v != V_OK && c.why == WHY_OK) c.why = w;
    };
    // (lot 82) prepared on the console for this translator and driver: the driver's part is in its cache, and what play still
    // does, translate a program when first met, takes milliseconds, not the driver's seconds: no program cache is no problem then
    if (f.prepared && f.driver_known && f.driver_bytes) {
        if (!f.notes_present || !f.notes_ok || !f.notes) c.notes = V_OK, c.coverage = 0;
        else c.coverage = (int)(f.notes_covered * 100 / f.notes);
        return c;
    }
    if (!f.programs_present || !f.programs_magic || !f.index_present || !f.index_ok || !f.programs) {
        c.programs = V_FAIL;
        if (f.programs_present && f.programs_magic && f.programs_stamp != stamp) c.version = V_FAIL, worse(V_FAIL, WHY_VERSION);
        else worse(V_FAIL, WHY_PROGRAMS);
    } else if (f.programs_stamp != stamp) {
        c.version = c.programs = V_FAIL;
        worse(V_FAIL, WHY_VERSION);
    }
    if (!f.driver_known || !f.driver_bytes) c.driver = V_FAIL, worse(V_FAIL, WHY_DRIVER);
    if (!f.notes_present || !f.notes_ok || !f.notes) {
        c.notes = V_WARN, c.coverage = 0;
        worse(V_WARN, WHY_NOTES);
    } else {
        c.coverage = (int)(f.notes_covered * 100 / f.notes);
        if (f.notes_covered < f.notes && c.coverage < 98) c.notes = V_WARN, worse(V_WARN, WHY_COVERAGE);
    }
    return c;
}
/// The words for a reason (the home page's line and the Shaders page's)
inline Text why_text(Why w) {
    switch (w) {
    case WHY_PROGRAMS: return T_SH_WHY_PROGRAMS;
    case WHY_VERSION: return T_SH_WHY_VERSION;
    case WHY_DRIVER: return T_SH_WHY_DRIVER;
    case WHY_NOTES: return T_SH_WHY_NOTES;
    case WHY_COVERAGE: return T_SH_WHY_COVERAGE;
    default: return T_SH_WHY_OK;
    }
}

// ---- whether the screen shows ----
/// env: HOOKSHOT_PS5_LAUNCHER (RX_VK_LAUNCHER off the console), nullptr when unset; mode: launcher= (0 always, 1 only when
/// needed); r1: R1 held as the screen would show; back (lot 73): the in-game menu's Back to the Grapnel screen asked for it
/// (launcher_back.h: its file found), which shows it whatever the mode. *why: the reason, for the log
inline bool show(const char* env, bool console, int mode, Verdict v, bool r1, bool back, bool prep, const char** why) {
    if (env && *env == '0') return *why = "switched off by the development settings", false;
    if (env && *env) return *why = "asked by the development settings", true;
    if (!console) return *why = "off the console it shows only when asked", false;
    if (back) return *why = "asked from the game: its menu's Back to the Grapnel screen", true;
    if (mode != 1) return *why = "launch screen: always", true;
    if (prep) return *why = "only when needed: the shaders are to prepare (HOOKSHOT_PS5_PREPARE=1)", true;
    if (v == V_FAIL) return *why = "only when needed: the shader check found a problem", true;
    if (r1) return *why = "only when needed: R1 held", true;
    return *why = "only when needed: nothing to say", false;
}
inline bool show(const char* env, bool console, int mode, Verdict v, bool r1, bool back, const char** why) {
    return show(env, console, mode, v, r1, back, false, why);
}
inline bool show(const char* env, bool console, int mode, Verdict v, bool r1, const char** why) {
    return show(env, console, mode, v, r1, false, false, why);
}

// ---- the pages and their buttons ----
enum Page { HOME, SETTINGS, SHADERS, PREPARING };   // (PREPARING: lot 82, the console's own preparation under way)
// (no CONTINUE since lot 71: it went through the host's title skip, which taps A by itself through the title (host/
// oot_autoload.c); a button that plays a scripted run of inputs for the player is left out)
enum HomeItem { H_PLAY, H_SETTINGS, H_SHADERS, H_QUIT, H_N };
inline constexpr Text kHomeNames[H_N] = {T_PLAY, T_SETTINGS, T_SHADERS, T_QUIT};
inline constexpr Text kHomeHelp[H_N] = {T_HELP_PLAY, T_HELP_SETTINGS, T_HELP_SHADERS, T_HELP_QUIT};
enum Outcome { NONE, PLAY, CONTINUE, QUIT };
constexpr int kCountdownSeconds = 5;
constexpr int64_t kSecond = 1000000000LL;
/// One frame's presses (each true once, when it goes down; up to right repeat while held, as the in-game menu's arrows)
struct Input {
    bool up = false, down = false, left = false, right = false, confirm = false, back = false, start = false, l1 = false,
         r1 = false, triangle = false;
    bool any() const { return up || down || left || right || confirm || back || start || l1 || r1 || triangle; }
};
struct State {
    Page page = HOME;
    int home = H_PLAY;
    int tab = TAB_PICTURE;
    int row[TAB_N] = {};
    bool counting = false;       // (the countdown runs: from start_ns, until any press)
    int64_t start_ns = 0;
    bool quit_armed = false;     // (QUIT pressed once: the next confirm quits)
    Outcome outcome = NONE;
    // (lot 82) the Shaders page's Prepare button (HOOKSHOT_PS5_PREPARE=1 and a census beside the game), above Ask the driver:
    // the button chosen there, 0 Prepare, 1 Ask the driver
    bool prep_button = false;
    int shaders_sel = 0;
};
/// What a press asks of the caller, which owns the values: a setting changed by dir, set back to its default, the driver asked
enum ActionKind { A_NONE, A_STEP, A_DEFAULT, A_DRIVER_CHECK, A_PREPARE, A_PREPARE_STOP };   // (the last two: lot 82)
struct Action {
    ActionKind kind = A_NONE;
    int setting = -1, dir = 0;
};
/// The screen shown at now. No countdown (lot 71: the player decides whether and when the game starts, the screen never does):
/// nothing starts the game but the player
inline void begin(State& s, int64_t now, Verdict v) {
    s = State{};
    s.start_ns = now;
    s.counting = false;
    if (v == V_FAIL) s.home = H_SHADERS;   // (a problem: the Shaders button chosen, the reason under it)
}
/// Whole seconds left on the countdown (rounded up), 0 when it ran out; -1 when it does not run
inline int countdown_left(const State& s, int64_t now) {
    if (!s.counting) return -1;
    const int64_t left = (int64_t)kCountdownSeconds * kSecond - (now - s.start_ns);
    return left <= 0 ? 0 : (int)((left + kSecond - 1) / kSecond);
}
/// One frame: the presses, at now. Sets s.outcome when the game is to start (or the title to close); returns what the caller
/// does to the settings. held: whether the setting under the selection is held by a development switch (left alone then)
inline Action step(State& s, const Input& in, int64_t now, bool selected_held = false) {
    Action a;
    if (s.outcome != NONE) return a;
    if (in.any()) s.counting = false;   // (any button stops the countdown, the one that starts the game too)
    else if (s.counting && countdown_left(s, now) == 0) return s.outcome = PLAY, a;
    // (lot 82) while the shaders are prepared nothing starts the game: Options and back stop the preparation (Later), and the
    // title then starts again with a heap of its own (vk_launcher.cpp)
    if (s.page == PREPARING) {
        if (in.back || in.start) a.kind = A_PREPARE_STOP;
        return a;
    }
    if (in.start) return s.outcome = PLAY, a;   // (Options: play now, from any page)
    switch (s.page) {
    case HOME:
        if (in.up || in.down) {
            s.home = (s.home + (in.down ? 1 : H_N - 1)) % H_N;
            s.quit_armed = false;
        }
        if (in.triangle) s.page = SETTINGS, s.quit_armed = false;
        if (in.back) s.quit_armed = false;
        if (in.confirm) {
            switch (s.home) {
            case H_PLAY: s.outcome = PLAY; break;
            case H_SETTINGS: s.page = SETTINGS; break;
            case H_SHADERS: s.page = SHADERS; break;
            case H_QUIT:
                if (s.quit_armed) s.outcome = QUIT;
                else s.quit_armed = true;
                break;
            }
        }
        break;
    case SETTINGS: {
        if (in.back) {
            s.page = HOME;
            break;
        }
        if (in.l1 || in.r1) s.tab = (s.tab + (in.r1 ? 1 : TAB_N - 1)) % TAB_N;
        const int n = tab_count(s.tab);
        int& r = s.row[s.tab];
        if (n > 0) {
            if (in.up) r = (r + n - 1) % n;
            if (in.down) r = (r + 1) % n;
            const int k = tab_setting(s.tab, r);
            if (k >= 0 && !selected_held) {
                if (in.left || in.right || in.confirm) a.kind = A_STEP, a.setting = k, a.dir = in.left ? -1 : 1;
                else if (in.triangle && kSettings[k].fallback != kNoDefault) a.kind = A_DEFAULT, a.setting = k;
            }
        }
        break;
    }
    case SHADERS:
        if (in.back) s.page = HOME;
        else if (s.prep_button && (in.up || in.down)) s.shaders_sel = 1 - s.shaders_sel;
        else if (in.confirm) a.kind = s.prep_button && s.shaders_sel == 0 ? A_PREPARE : A_DRIVER_CHECK;
        break;
    case PREPARING:
        break;
    }
    return a;
}
/// (lot 82) The preparation begun: its page shown (the screen's caller started it)
inline void preparing(State& s) { s.page = PREPARING, s.quit_armed = false; }
/// The word the home page gives the Shaders button: the shader check's, or the preparation's when it is to do or to resume
inline Text shaders_word(Verdict v, console_prep::Offer o) {
    if (o == console_prep::Offer::kNeeded) return T_TO_PREPARE;
    if (o == console_prep::Offer::kResume) return T_TO_RESUME;
    return v == V_OK ? T_READY : v == V_WARN ? T_TO_COMPLETE : T_PROBLEM;
}
/// The Shaders page's line for the preparation, and the reason under the verdict when there is one to say
inline Text prep_reason(console_prep::Offer o) {
    switch (o) {
    case console_prep::Offer::kNeeded: return T_SH_PREP_WHY_NEEDED;
    case console_prep::Offer::kResume: return T_SH_PREP_WHY_RESUME;
    case console_prep::Offer::kOptional: return T_SH_PREP_WHY_OPTIONAL;
    case console_prep::Offer::kDone: return T_SH_PREP_WHY_DONE;
    case console_prep::Offer::kNoCensus: return T_SH_PREP_WHY_NO_CENSUS;
    default: return T_SH_WHY_OK;
    }
}
/// What the preparation's page says when it ended (console_prep::End), before the title starts again
inline Text prep_end_text(console_prep::End e) {
    switch (e) {
    case console_prep::End::kFinished: return T_PREP_DONE;
    case console_prep::End::kMemory: return T_PREP_LOW_MEMORY;
    case console_prep::End::kStall: return T_PREP_STALL;
    case console_prep::End::kNothing: return T_PREP_NOTHING;
    default: return T_PREP_STOPPED;
    }
}
/// The setting under the selection on the Settings page (an index in kSettings), -1 elsewhere
inline int selected_setting(const State& s) {
    return s.page == SETTINGS ? tab_setting(s.tab, s.row[s.tab]) : -1;
}

// ---- (lot 74) the in-game menu on the console: the same lists as this screen ----
// With HOOKSHOT_PS5_MENU_CONSOLE=1 in hookshot_env.txt the in-game menu (vk_menu.cpp console_menu) shows on the console the
// settings this screen offers (kSettings), stepped through the same values (next_value), and the rows of its own below; every
// other row is hidden there, a row a merge of Longshot adds too, until it is listed here. At the start the hidden rows that change
// the game take the console's values (kConsolePinned), each one said in the log, and the options file is written again once.
// Without the switch the menu is as it was. The choice made with the player: notes-agents/menu-en-jeu-console.md
inline constexpr const char* kConsoleMenuSwitch = "HOOKSHOT_PS5_MENU_CONSOLE";
/// The in-game menu's rows the console shows beside kSettings', by their key in hookshot_options.ini: they make sense in play
/// only (the FPS counter's corner and size, the camera's centering, the gyroscope's dead zone, the autosaves)
inline constexpr const char* kConsoleMenuOnly[] = {
    "fps_counter_position", "fps_counter_size", "disable_camera_centering", "motion_deadzone", "autosave:",
};
/// And its rows with no value, by their name in vk_menu.cpp rows[]: the Motion page, Quit (lot 74: it closes the title on the
/// console), Back to the Grapnel screen (lot 73, with its own switch; its name since lot 80). Not Restart (the console has no
/// restart of its own: Back to the Grapnel screen starts the title again), not the Controls page (its grid is the PC's keys:
/// Layout preset is on the Controls tab instead), nor the PC's tools (Bug report, Save my pipeline list, Set up for a weak
/// laptop...)
inline constexpr const char* kConsoleMenuPages[] = {"Motion", "Quit", "Back to the Grapnel screen"};
/// (lot 86) The microphone's rows, on the Audio tab under Volume: what the ocarina hears when the game's microphone option is on,
/// player 1's DualSense microphone (host/posix/ps5_mic.h). Shown with HOOKSHOT_PS5_MIC=1 only, the switch that turns the
/// console's microphone on: without it the game hears silence and the rows would offer nothing. By their key (Microphone,
/// Microphone gain) and their name (Test microphone, no value)
inline constexpr const char* kConsoleMicSwitch = "HOOKSHOT_PS5_MIC";
inline constexpr const char* kConsoleMicKeys[] = {"microphone", "microphone_gain"};
inline constexpr const char* kConsoleMicPages[] = {"Test microphone"};
/// The console's values of the rows its menu hides that change the game, as the console has played (its options file of
/// 22/11, build/lot71/console-apres-71-A/hookshot_options.ini). Not output_res (a size, which the host does not read with the
/// handheld profile) nor ultrawide (one 16:9 TV): vk_menu.cpp save writes them back as read. The debug tools (debug_tools) come
/// back with HOS_OOT_CHEATS=1, for our trials, whatever the row
struct Pinned {
    const char* key;
    int value;
};
inline constexpr Pinned kConsolePinned[] = {
    {"handheld_profile", 1}, {"quality_preset", 1},  {"dlss", 0},          {"resolution", 1},       {"sharpness", 0},
    {"upscaling", 1},        {"frame_pacing", 1},    {"game_speed", 1},    {"vsync", 0},            {"hdr", 0},
    {"skip_title_screen", 0}, {"sampler_cache", 0},  {"robust_buffers", 0}, {"astc_as_rgba8", 0},  {"shadow_resolution", 0},
    {"anisotropic_filtering", 4}, {"field_of_view", 100}, {"crt_filter", 0}, {"audio_output", 0}, {"debug_tools", 0},
    {"button_prompts", 0},
};
/// And memory_budget, not a row: Minimum's (1), whose mip trimming the console has played with
constexpr int kConsoleBudget = 1;
inline bool listed(const char* const* list, size_t n, const char* s) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return true;
    return false;
}
/// Whether the console's menu shows the row of this key (a row with a value); mic: HOOKSHOT_PS5_MIC=1, the microphone's rows too
inline bool console_menu_key(const char* key, bool mic = false) {
    return setting_of(key) >= 0 || listed(kConsoleMenuOnly, sizeof kConsoleMenuOnly / sizeof *kConsoleMenuOnly, key) ||
           (mic && listed(kConsoleMicKeys, sizeof kConsoleMicKeys / sizeof *kConsoleMicKeys, key));
}
/// Whether it shows the row of this name (a row with no value: a page or an action)
inline bool console_menu_page(const char* name, bool mic = false) {
    return listed(kConsoleMenuPages, sizeof kConsoleMenuPages / sizeof *kConsoleMenuPages, name) ||
           (mic && listed(kConsoleMicPages, sizeof kConsoleMicPages / sizeof *kConsoleMicPages, name));
}
/// The console's value of the row of this key, when its menu hides it and it is put back: nullptr otherwise
inline const Pinned* console_pinned(const char* key) {
    for (const Pinned& p : kConsolePinned)
        if (!strcmp(p.key, key)) return &p;
    return nullptr;
}

}  // namespace mkwvk::launcher
