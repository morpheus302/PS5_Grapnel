#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Grapnel builder's window: a step-by-step assistant (Tkinter) over the same core as the command line (grapnel_core.py).

    Windows: Grapnel.exe, in the builder's folder (it starts python\\pythonw.exe on this file)
    Linux:   python3 -I scripts/grapnel-builder/grapnel_gui.py [--toolchain D] [--llvm D] [--python P] [--prompt-python P]

The pages, as the player validated them (notes-agents/assistant-constructeur.md, 23/11/2026):
    1 Game      the .nup, checked as soon as it is chosen: Next only for the right game and update
      (copy)    (a .nup whose game files are stored compressed only; never for a normal one) a page of its own after it: what
                happens (the console reads only flat files: the builder makes a flat copy, here, once, and that copy is what
                the console gets), its folder (the output folder by default, another one with Browse), the disk's room for it
                checked first (what to do when there is too little), then the copy (grapnel_build.py --flatten) with its bar;
                Next once it is made and checked (or taken again: one made before from the same file)
    2 Options   the output folder; the controller button icons (on by default); Advanced: the compile's memory (0 = automatic)
    3 Check     runs by itself on arrival: a list of green ticks (tools, Python, ZBIC, a test compile, the processor, the .nup,
                disk, memory, paths); a problem says what to do; Next only when all is green (a processor without x86-64-v3 is
                a note, in blue, not a problem)
    4 Game      the build (about 30 min): a bar, the step, the time left; "Chain the shaders automatically" (on by default)
    5 Shaders   the driver's cache prepared on this PC (the core's step 7); where this PC does not (a processor without
                x86-64-v3, AVX2: the check's Processor line says so, and the build sets up by itself the console's own
                preparation, at its first start; or a toolchain without prepare/: the page's fallback button), the page
                says what was done
    6 Console   the title sent to the console over FTP (grapnel_send.py), to its internal storage or its extension disk
                (/mnt/ext1), or how to copy the folders by hand
On every page: the steps along the top (done ones ticked) with Back; at the bottom Open the folder, Copy the report, the log
(hidden until asked), a file's CRC32, Cancel, Next. A game already built in the output folder: the first page offers to go
straight to the shaders or to the console.

The build runs in a process of its own (grapnel_build.py --events json); on Windows a job object holds it and every program it
starts, so Cancel, or the window's end, stops them all. The PC is kept awake while it builds. A stopped build carries on where
it was when started again (the core reads, translates and compiles again only what changed).
"""
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))      # the repository (app\ in the builder's folder)
TOP = os.path.dirname(ROOT)                        # the builder's folder (Windows package)
WINDOWS = os.name == "nt"
sys.path.insert(0, HERE)

import grapnel_send  # noqa: E402


def probe_places(ip, port=grapnel_send.FTP_PORT):
    """grapnel_send.probe(), and when the console's FTP server answers: its extension disks and where the title and the game
    file are (volumes: None when that could not be asked)"""
    found = grapnel_send.probe(ip, port=port)
    found["volumes"] = None
    if found["ftp"]:
        try:
            c = grapnel_send.Console(ip, port, root=os.environ.get("GRAPNEL_TEST_CONSOLE_ROOT", ""), timeout=10.0)
            try:
                found["volumes"] = grapnel_send.volumes(c)
                found["where"] = grapnel_send.where(c, found["volumes"])
            finally:
                c.close()
        except (OSError, EOFError, grapnel_send.ftplib.Error, grapnel_send.SendError):
            found["volumes"] = None
    return found
import tkinter as tk  # noqa: E402
from tkinter import filedialog, messagebox, ttk  # noqa: E402

DEEPEST = 140        # (the work folder's deepest path, relative, with room: 124 characters measured on 23/11/2026)
DISK_GB = 12         # (a whole build: ~6 GB of work and title, the shaders' files and cache ~1 GB, with room)
MEMORY_GB = 7.5      # (under this much memory the build is very slow: the translation alone wants 4 GB)
# (the core's steps' share of the game's build, step 4 of the assistant: Linux ~26 min, the player's Windows ~37 min)
GAME_WEIGHT = {1: 1, 2: 15, 3: 70, 4: 1, 5: 3, 6: 8, 8: 1}
GAME_MINUTES = 35    # (the first estimate, before the build has gone far enough to measure itself)
PREPARE_STEP = 7
LEFT = re.compile(r"\(([^;()]+) left")
PAGES = ("game", "options", "check", "build", "shaders", "console")

# ---- the texts, French and English ----

T = {
    "fr": {
        "title": "Grapnel : constructeur",
        "subtitle": "Fabrique le titre PS5 Grapnel (GRPN00001) à partir de votre propre fichier du jeu.",
        "steps": ("Jeu", "Options", "Vérifier", "Jeu", "Shaders", "Console"),
        "back": "« Précédent", "next": "Suivant »", "cancel": "Annuler", "build_btn": "Construire »", "finish": "Terminer",
        "open_out": "Ouvrir le dossier", "copy_report": "Copier le rapport", "log_show": "Journal +", "log_hide": "Journal –",
        "crc_tool": "CRC32 d'un fichier…", "browse": "Parcourir…",
        "p_game": "Étape 1 sur 6 : votre jeu",
        "p_game_text": "Choisissez le fichier .nup de Zelda Ocarina of Time (Nintendo Switch 2 Edition) avec sa mise à jour v131072, "
                       "celui que la console lit sous le nom game.nup. Rien n'en sort de ce PC.",
        "nup": "Fichier du jeu (.nup)", "nup_ok": "✓ Le bon jeu, avec sa mise à jour v131072.",
        "nup_checking": "Lecture du fichier…", "nup_none": "Aucun fichier choisi.",
        "nup_flat": "\nUne partie de ses fichiers est compressée ({what}) : la page suivante en fait la copie que la console sait lire.",
        "what_coded": "{coded} fichiers sur {files}", "what_framed": "ses fichiers sont des trames zstd",
        "step_flat": "copie",
        "flat_banner": "Compression détectée",
        "p_flat": "Étape 1 sur 6 (suite) : une copie de votre jeu pour la console",
        "flat_text": "Votre fichier du jeu garde une partie de ses fichiers compressés ({what}) : certains outils de dump "
                     "l'écrivent ainsi. La construction s'en accommode, mais la console lit le fichier du jeu tel quel et ne "
                     "sait pas ouvrir ces fichiers-là : le jeu s'arrêterait dès « Jouer ».\n\n"
                     "Le constructeur en fait donc, sur ce PC et une seule fois, une copie « à plat » : le même jeu, chaque "
                     "fichier tel quel. C'est cette copie, et non votre dump, que la console recevra (étape 6) ou que vous y "
                     "copierez à la main. Votre dump n'est pas modifié, et les prochaines constructions reprendront la copie.",
        "flat_cost": "Ce que cela coûte : {gb} Go sur le disque du dossier ci-dessous, et quelques minutes sur un SSD (davantage "
                     "sur un disque dur) : la copie est écrite, puis relue en entier pour la vérifier.",
        "flat_dir": "Dossier de la copie", "flat_reading": "Lecture du fichier du jeu…",
        "flat_space_ok": "✓ {free} Go libres sur ce disque : assez pour la copie ({need} Go avec une marge).",
        "flat_space_low": "✗ Pas assez de place : ce disque a {free} Go libres, la copie en demande {need} avec une marge. Libérez-y "
                          "{more} Go (la corbeille, les téléchargements, une ancienne copie du jeu dont vous n'avez plus "
                          "besoin…), ou choisissez avec « Parcourir… » un dossier sur un autre disque, puis « Vérifier la place ».",
        "flat_space_again": "Vérifier la place",
        "flat_done_before": "✓ La copie faite avant à partir de ce même fichier est là : {path} ({gb} Go). Elle est reprise : "
                            "rien à refaire.",
        "flat_partial": "Une copie commencée est là ({done} Go sur {gb}) : elle reprendra où elle s'était arrêtée.",
        "flat_discard": "Effacer la copie commencée",
        "flat_other": "✗ Ce dossier a déjà un game.nup que le constructeur n'a pas fait (ou c'est votre dump lui-même) : "
                      "choisissez un autre dossier.",
        "flat_stale": "Une ancienne copie, d'un autre fichier ou d'une autre version, est là : elle sera remplacée.",
        "flat_start": "Faire la copie »",
        "flat_made": "✓ La copie est faite et vérifiée, en {m} min : {path} ({gb} Go, {n} fichiers décompressés). C'est elle que "
                     "la console recevra.",
        "corrupt_banner": "Fichiers corrompus détectés",
        "corrupt_text": "{what} dans votre dump ne sont pas ceux du jeu : leur empreinte (dans le fichier lui-même) n'est pas celle "
                        "d'un dump sain, l'outil de dump les a mal lus.",
        "corrupt_n": "{n} fichiers", "corrupt_1": "Un fichier", "corrupt_missing": " Et {m} fichiers du jeu manquent.",
        "corrupt_missing_only": "{m} fichiers du jeu manquent dans votre dump : l'outil de dump ne les a pas écrits.",
        "corrupt_more": "… et {k} autres (tous dans le journal et le rapport)",
        "corrupt_effect": "Là où le jeu s'en sert, il peut lui manquer une texture, un son ou une voix, ou il peut s'arrêter. "
                          "Conseil : refaites le dump de votre jeu (ou copiez-le à nouveau depuis la console). Vous pouvez "
                          "continuer quand même.",
        "corrupt_kinds": {"program": "programme du jeu", "texture": "texture", "model": "modèle 3D", "terrain": "terrain",
                          "voice": "voix", "movie": "vidéo", "sound": "son", "shader": "shader", "font": "police",
                          "ui": "interface", "text": "texte", "ai": "comportement", "physics": "physique", "effect": "effet",
                          "event": "événement", "setting": "réglage", "pack": "paquet d'objets", "other": "fichier du jeu"},
        "corrupt_missing_tag": "manque",
        "corrupt_short": "La liste et ce que cela peut faire sont sur la page précédente (« Jeu »). Refaites le dump si vous le "
                         "pouvez, ou continuez quand même.",
        "flat_progress": {"the flat copy": "la copie", "checked": "vérifiée", "taken again": "reprise", "checking:": "vérification :",
                          " of ": " sur ", "GB": "Go"},
        "resume": "Le jeu est déjà construit dans « {out} ».",
        "resume_shaders": "Aller aux shaders", "resume_console": "Aller à la console",
        "p_options": "Étape 2 sur 6 : options",
        "out": "Dossier de sortie",
        "out_text": "Le dossier où le titre et les données seront faits (environ 7 Go, dont le travail de la construction). "
                    "Un chemin court et simple est le plus sûr, par exemple C:\\Grapnel.",
        "icons": "Faire les icônes de manette (à partir de vos fichiers du jeu)",
        "advanced": "Avancé +", "advanced_hide": "Avancé –",
        "memory": "Mémoire pour la compilation (Go, 0 = automatique)",
        "out_free": "{gb:.0f} Go libres sur ce disque.", "out_resume": " Une construction précédente est là : elle reprendra où elle "
                                                                     "s'était arrêtée.",
        "out_elsewhere": "Une construction précédente est dans « {other} » : ici, tout serait refait (environ une heure).",
        "out_go_back": "Reprendre dans ce dossier",
        "p_check": "Étape 3 sur 6 : vérifier ce PC",
        "check_text": "La vérification se fait toute seule. Tout doit être vert pour continuer.",
        "check_again": "Vérifier à nouveau",
        "rows": {"llvm": "Compilateur (LLVM 19.1.7)", "toolchain": "Outils de la console", "native": "Outil de signature",
                 "python": "Python et ses paquets", "zbic": "Décompression du jeu (ZBIC)", "compile": "Compilation d'essai pour la PS5",
                 "cpu": "Processeur", "prepare": "Préparation des shaders sur ce PC", "nup": "Fichier du jeu",
                 "disk": "Place sur le disque", "memory": "Mémoire", "paths": "Chemins"},
        "row_wait": "…", "check_running": "Vérification en cours…", "check_ok": "Tout est prêt.",
        "check_bad": "Un point n'est pas en ordre : voyez la ligne en rouge.",
        "prep_missing": "absente : la console préparera (étape 5)",
        "cpu_ok": "ok",
        "cpu_console": "sans AVX2 : les shaders seront préparés par la console, à son premier lancement (environ une heure sur "
                       "une PS5 standard), et non sur ce PC. Rien à faire : la construction le met en place.",
        "prep_cpu": "par la console, à son premier lancement (processeur sans AVX2)",
        "disk_ok": "{gb:.0f} Go libres", "mem_ok": "{total:.0f} Go ({avail:.0f} Go libres)", "mem_low": "{total:.0f} Go : très lent",
        "paths_ok": "courts et lisibles",
        "p_build": "Étape 4 sur 6 : construire le jeu",
        "build_text": "Environ 30 minutes (jusqu'à une heure sur un petit PC). Le PC reste éveillé ; vous pouvez réduire la fenêtre, "
                      "mais ne la fermez pas.",
        "chain": "Enchaîner les shaders automatiquement",
        "left": "environ {m} min restantes", "left_soon": "moins d'une minute", "elapsed": "écoulé : {t}",
        "core_steps": {"Reading the game's program": "Lecture du programme du jeu", "Translating it to C": "Traduction en C",
                       "Compiling and linking for the console": "Compilation pour la console",
                       "Putting the title and data folders together": "Dossiers du titre et des données",
                       "Making the button icons": "Icônes de manette", "Listing the game's shaders": "Liste des shaders du jeu",
                       "Preparing the shaders on this PC": "Préparation des shaders", "Writing the notes": "Notices"},
        "build_done": "Le jeu est construit. eboot.bin : {bytes} octets, CRC32 {crc} (0x{crc:08X}).",
        "build_again": "Reconstruire",
        "p_shaders": "Étape 5 sur 6 : construire les shaders",
        "shaders_text": "Le PC compile pour la console tous les shaders du jeu dans le cache de son pilote : la console n'aura "
                        "rien à compiler en jouant, elle ne fera que vérifier.",
        "shaders_start": "Préparer les shaders »", "shaders_done": "Les shaders sont prêts : {entries} entrées dans le cache.",
        "shaders_cpu": "Ce PC ne prépare pas les shaders : son processeur n'a pas l'AVX2 (x86-64-v3), dont l'outil de préparation "
                       "a besoin. La console les prépare donc elle-même, à son premier lancement : environ une heure sur une PS5 "
                       "standard (sur un tel PC, la préparation durerait à peu près autant). « Plus tard » l'arrête, et le "
                       "lancement suivant reprend où elle en était.\n\nLa construction le met en place d'elle-même : la liste des "
                       "shaders du jeu (faite à l'étape 4 à partir de votre jeu) et le réglage qui la propose vont à côté du jeu, "
                       "dans data\\grapnel.",
        "shaders_stopped": "La préparation sur ce PC s'est arrêtée sur une instruction que son processeur n'a pas. La console "
                           "prépare donc les shaders elle-même, à son premier lancement : environ une heure sur une PS5 standard "
                           "(« Plus tard » l'arrête, et le lancement suivant reprend où elle en était). La construction l'a mis en "
                           "place : la liste des shaders du jeu et le réglage qui la propose sont dans data\\grapnel.\n\n"
                           "Signalez-le avec le nom du processeur (issues de Grapnel) : ce PC aurait dû passer directement par la "
                           "console.",
        "shaders_no_tool": "Le dossier d'outils donné n'a pas la préparation des shaders pour ce système (son dossier prepare/).\n\n",
        "shaders_console": "Le secours : la console les prépare elle-même, au premier lancement, environ une heure sur une PS5 "
                           "standard (« Plus tard » l'arrête et elle reprendra). Pour cela, le bouton ci-dessous met à côté du jeu la "
                           "liste de ses shaders (faite à l'étape 4 à partir de votre jeu) et le réglage qui la propose. Sans cela, "
                           "le jeu compilera les shaders en jouant (saccades).",
        "shaders_console_wait": "La construction le met en place à sa fin.",
        "fallback_btn": "Préparer pour la console (secours)",
        "fallback_done": "C'est prêt : copiez data\\grapnel avec ses deux fichiers de plus (étape 6). Au premier lancement, "
                         "la console proposera « Préparer les shaders ».",
        "fallback_missing": "La liste des shaders manque : relancez la construction (étape 4).",
        "p_console": "Étape 6 sur 6 : la console",
        "console_intro": "Facultatif : envoyer le titre à la console par le réseau. Vos parties sont d'abord copiées sur ce PC. "
                         "Sur la console, seuls le dossier du titre (GRPN00001), celui du jeu et /data/grapnel sont touchés : "
                         "jamais les autres jeux.",
        "console_place": "Installer le titre et le fichier du jeu sur :",
        "place_internal": "le stockage interne (/data)",
        "place_ext": "le disque d'extension ({vol})",
        "console_vols": "Disque d'extension trouvé : {vols}. La place libre n'est pas connue (le serveur FTP ne la donne pas).",
        "console_vol_none": "Aucun disque d'extension sur la console : le titre ira sur le stockage interne.",
        "console_vol_later": "Le disque d'extension sera cherché à l'envoi, une fois le serveur FTP lancé.",
        "console_title_at": "Le titre est déjà sur {where}.",
        "console_no_volume": "Le disque d'extension {vol} n'est pas sur la console : rien n'a été envoyé. Branchez-le, ou "
                             "choisissez le stockage interne.",
        "console_elsewhere": "Le titre est aussi sur {old} : une fois l'envoi relu, la fenêtre proposera de le retirer.",
        "console_move_title_title": "Retirer l'ancien titre ?",
        "console_move_title": "Le titre est maintenant sur {new}, envoyé et relu.\n\nUn ancien exemplaire est encore là :\n{old}\n\n"
                              "Le retirer ? Deux titres GRPN00001 gêneraient ShadowMountPlus. Vos parties sont dans "
                              "/data/grapnel : elles n'y sont pas, et elles restent.",
        "console_move_nup_title": "Effacer l'ancienne copie du jeu ?",
        "console_move_nup": "Le fichier du jeu est maintenant sur {new}, avec sa taille.\n\nUne ancienne copie est encore là :\n"
                            "{old} ({gb:.1f} Go)\n\nL'effacer ? Cela libère {gb:.1f} Go.",
        "console_removed": "Retiré : {what}.",
        "console_kept": "Gardé (à votre demande) : {what}.",
        "console_ip": "Adresse IP de la console", "console_test": "Tester la connexion",
        "console_testing": "Test de la connexion…",
        "console_ftp": "✓ La console répond : son serveur FTP est prêt (port 2121).",
        "console_elfldr": "✓ La console répond (chargeur d'ELF, port 9021) : l'envoi y lancera d'abord le serveur FTP ftpsrv.",
        "console_no_ftpsrv": "La console répond (chargeur d'ELF), mais ce paquet n'a pas ftpsrv : lancez un serveur FTP sur la "
                             "console (port 2121), puis testez à nouveau.",
        "console_none": "Rien ne répond à cette adresse (ports 2121 et 9021). La console est-elle allumée, débridée, sur le même "
                        "réseau, avec son chargeur d'ELF ou un serveur FTP lancé ?",
        "console_nup": "Envoyer aussi le fichier du jeu (game.nup, 26 Go), s'il n'y est pas déjà avec sa taille",
        "console_nup_flat": "Envoyer aussi le fichier du jeu : sa copie à plat (game.nup, {gb} Go), si elle n'y est pas déjà avec "
                            "sa taille",
        "console_manual_flat": "Votre dump garde des fichiers compressés : pour le fichier du jeu, copiez la copie à plat\n"
                               "{path}\net non votre dump (la console ne saurait pas le lire).",
        "console_send": "Envoyer à la console »", "console_not_built": "Construisez d'abord le jeu (étape 4).",
        "console_ftpsrv_sent": "Le serveur FTP est envoyé au chargeur d'ELF de la console…",
        "console_ftp_late": "Le serveur FTP envoyé ne répond pas sur le port 2121.",
        "console_saving": "Copie de vos parties sur ce PC…",
        "console_saved": "Vos parties ({n} fichiers) sont copiées dans {dest}.",
        "console_ask_title": "Remplacer sur la console ?",
        "console_ask": "La console a déjà :\n\n{what}\n\nCes dossiers seront mis à jour avec la nouvelle construction "
                       "(vos parties y restent ; elles sont aussi copiées sur ce PC{saved}). Continuer ?",
        "console_ask_saved": " : {dest}",
        "console_rate": "{mb:.1f} Mo/s, environ {m} min restantes",
        "console_verifying": "Relecture : les tailles et le CRC32 de l'eboot…",
        "console_ok": "✓ Envoyé et relu : chaque fichier a sa taille, et l'eboot son CRC32 ({crc}).\n\n"
                      "L'enregistrement : ShadowMountPlus enregistre de lui-même ce qu'il trouve dans {homebrew}, une "
                      "minute environ après la fin de l'envoi ; le titre « Grapnel » apparaît alors sur l'écran d'accueil. Sans "
                      "ShadowMountPlus, enregistrez {homebrew}/GRPN00001 avec votre outil habituel.",
        "console_bad": "✗ La relecture ne correspond pas :\n{what}\n\nRenvoyez : ce qui est juste est gardé.",
        "console_failed": "✗ L'envoi s'est arrêté : {e}\nRenvoyer reprendra là où il en était.",
        "console_cancelled": "Arrêté. Renvoyer reprendra là où c'en était.",
        "console_manual": "Ou à la main (FTP ou gestionnaire de fichiers de la console) :\n\n"
                        "1. homebrew\\GRPN00001  →  /data/homebrew/GRPN00001, puis enregistrez le titre.\n"
                        "2. data\\grapnel  →  /data/grapnel (si la console a déjà /data/hookshot : voyez la notice).\n"
                        "3. Votre fichier du jeu  →  /data/grapnel/game.nup.\n"
                        "Sur le disque d'extension : homebrew\\GRPN00001  →  /mnt/ext1/homebrew/GRPN00001, le fichier du jeu  →  "
                        "/mnt/ext1/grapnel/game.nup, et la ligne HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup dans "
                        "/data/grapnel/hookshot_env.txt.\n\n"
                        "Ces dossiers viennent de votre jeu : ne les partagez jamais. Tout est expliqué dans INSTALL.fr.txt.",
        "open_install": "Ouvrir la notice", "no_install": "La notice n'existe pas encore : construisez d'abord.",
        "confirm_cancel": "Arrêter ? Ce qui est fait est gardé : relancer reprendra là.",
        "confirm_quit": "Une construction est en cours. Fermer la fenêtre l'arrête. Fermer quand même ?",
        "cancelled": "Arrêté. Relancer reprendra où c'était.",
        "warn_memory": "Ce PC a {total:.0f} Go de mémoire : il en faut 8 au moins, 16 pour aller vite. La construction peut être très "
                       "lente. Continuer ?",
        "err_disk": "Le disque du dossier de sortie a {gb:.1f} Go libres ; il en faut environ {need}.",
        "err_path": "Le chemin du dossier de sortie est trop long pour Windows ({n} caractères ; {max} au plus ici). Choisissez un "
                    "dossier plus court, par exemple C:\\Grapnel.",
        "err_chars": "Le chemin « {path} » a des caractères que certains outils ne lisent pas sur ce PC ({chars}). Choisissez un "
                     "dossier au nom simple, par exemple C:\\Grapnel.",
        "err_not_dir": "Le dossier de sortie ne peut pas être créé : {e}",
        "err_title": "Grapnel", "err_details": "Détails (le journal complet : {log}) :",
        "crc_pick": "Choisissez un fichier (eboot.bin par exemple)",
        "crc_result": "{name}\n{bytes} octets\nCRC32 : {crc} (0x{crc:08X})\n\n(copié dans le presse-papiers)",
        "report_copied": "Le rapport est copié : collez-le dans votre message.",
        "worker": "Le constructeur s'est arrêté sans dire pourquoi (code {rc}). Le journal le dit peut-être.",
        "errors": {
            "llvm": "Le compilateur livré avec le constructeur ne répond pas. Décompressez à nouveau tout le zip dans un dossier à vous.",
            "toolchain": "Les outils de la console sont incomplets ou abîmés (un antivirus a peut-être retiré un fichier). "
                         "Décompressez à nouveau tout le zip.",
            "python": "Le Python du constructeur n'a pas ses paquets. Décompressez à nouveau tout le zip.",
            "python-icons": "Le Python du constructeur n'a pas les paquets des icônes. Décompressez à nouveau tout le zip, ou "
                            "décochez les icônes (étape 2).",
            "zbic": "Le programme qui décompresse le jeu ne marche pas ici (un antivirus l'a peut-être bloqué : voyez LISEZMOI).",
            "nup-folder": "Vous avez choisi un dossier : choisissez le fichier .nup lui-même.",
            "nup-missing": "Le fichier du jeu est introuvable : choisissez-le à nouveau.",
            "nup-not-nup": "Ce fichier n'est pas un paquet .nup.",
            "nup-damaged": "Le fichier .nup est incomplet ou abîmé : copiez-le à nouveau depuis votre console.",
            "nup-wrong-game": "Ce .nup n'est pas le bon jeu ou pas la bonne version : il faut Zelda Ocarina of Time (Nintendo Switch "
                              "2 Edition) avec sa mise à jour v131072.",
            "prepare-cpu": "La préparation des shaders sur ce PC s'est arrêtée sur une instruction que son processeur n'a pas : la "
                           "console les préparera elle-même à son premier lancement (environ une heure sur une PS5 standard), "
                           "la construction le met en place. Signalez-le avec le nom du processeur (issues de Grapnel).",
            "prepare-console": "La liste des shaders manque pour la préparation par la console : relancez la construction "
                               "(étape 4).",
            "nup-codec": "Ce .nup range des fichiers du jeu sous une forme que le constructeur ne connaît pas : refaites le dump "
                         "avec un autre outil.",
            "flat-space": "Pas assez de place pour la copie du jeu : libérez de la place sur ce disque, ou choisissez un dossier "
                          "sur un autre disque.",
            "flat-other": "Le dossier choisi pour la copie a déjà un game.nup que le constructeur n'a pas fait : choisissez un autre "
                          "dossier.",
            "flat-failed": "Un fichier du jeu n'a pas pu être décompressé : votre dump est sans doute abîmé à cet endroit. Refaites "
                           "le dump, ou copiez-le à nouveau depuis la console.",
            "flat-verify": "La copie relue ne correspond pas à ce qui a été écrit (le disque ?). Refaites la copie ; si cela "
                           "recommence, essayez un autre disque.",
            "flat-cancelled": "Arrêté. La copie reprendra où elle en était.",
            "options": "Il manque le fichier du jeu ou le dossier de sortie.",
            "disk": "Pas assez de place sur le disque du dossier de sortie.",
            "tool-failed": "Un outil de la construction a échoué. Envoyez le journal avec votre rapport.",
            "lift-incomplete": "La traduction s'est arrêtée avant la fin, sans doute faute de mémoire. Fermez les autres programmes "
                               "et relancez.",
            "compile-incomplete": "La compilation s'est arrêtée avant d'avoir fait le titre. Relancez : elle reprendra là.",
            "flexible": "L'eboot ne tient pas dans la mémoire prévue sur la console : c'est au projet de le corriger. Envoyez le "
                        "journal.",
            "shader-stamp": "Une vérification interne a échoué (empreinte des shaders). Envoyez le journal.",
            "imports": "Une vérification interne a échoué (importations). Envoyez le journal.",
            "title": "Une vérification interne a échoué (identifiant du titre). Envoyez le journal.",
            "order": "Erreur interne du constructeur (ordre des étapes). Envoyez le journal.",
            "checkout": "Les fichiers du constructeur sont incomplets. Décompressez à nouveau tout le zip.",
            "census": "La liste des shaders du jeu n'a pas pu être faite. Envoyez le journal.",
            "census-wrong": "La liste des shaders du jeu est incomplète. Relancez ; si cela recommence, envoyez le journal.",
            "states": "La liste des pipelines n'a pas pu être faite. Envoyez le journal.",
            "prepare-tool": "L'outil de préparation des shaders manque ou est abîmé. Décompressez à nouveau tout le zip.",
            "prepare-platform": "L'outil de préparation des shaders ne marche pas encore sur ce système.",
            "prepare-stale": "L'outil de préparation n'est pas de la même version que le rendu : prenez un constructeur à jour.",
            "prepare-failed": "La préparation des shaders s'est arrêtée. Relancez : elle reprendra où elle en était.",
            "prepare-unfinished": "La préparation des shaders n'est pas allée au bout. Relancez : elle reprendra.",
            "prepare-cache": "Le cache des shaders fait n'est pas valable. Relancez ; si cela recommence, envoyez le journal.",
            "prepare-unstable": "La préparation des shaders ne se stabilise pas. Envoyez le journal.",
        },
    },
    "en": {
        "title": "Grapnel builder",
        "subtitle": "Makes the PS5 title Grapnel (GRPN00001) from your own copy of the game.",
        "steps": ("Game", "Options", "Check", "Game", "Shaders", "Console"),
        "back": "« Back", "next": "Next »", "cancel": "Cancel", "build_btn": "Build »", "finish": "Finish",
        "open_out": "Open the folder", "copy_report": "Copy the report", "log_show": "Log +", "log_hide": "Log –",
        "crc_tool": "A file's CRC32…", "browse": "Browse…",
        "p_game": "Step 1 of 6: your game",
        "p_game_text": "Choose the .nup of Zelda Ocarina of Time (Nintendo Switch 2 Edition) with its update v131072, the file the "
                       "console reads as game.nup. Nothing of it leaves this PC.",
        "nup": "Game file (.nup)", "nup_ok": "✓ The right game, with its update v131072.",
        "nup_checking": "Reading the file…", "nup_none": "No file chosen.",
        "nup_flat": "\nPart of its files are stored compressed ({what}): the next page makes the copy the console can read.",
        "what_coded": "{coded} files of {files}", "what_framed": "its files are zstd frames",
        "step_flat": "copy",
        "flat_banner": "Compression detected",
        "p_flat": "Step 1 of 6 (continued): a copy of your game for the console",
        "flat_text": "Your game file keeps part of its files compressed ({what}): some dump tools write it that way. The build "
                     "copes with it, but the console reads the game file as it is and cannot open those files: the game would "
                     "stop as soon as you press Play.\n\n"
                     "So the builder makes, on this PC and once only, a \"flat\" copy: the same game, every file as itself. That "
                     "copy, not your dump, is what the console gets (step 6) or what you copy to it by hand. Your dump is not "
                     "changed, and the next builds take the copy again.",
        "flat_cost": "What it costs: {gb} GB on the disk of the folder below, and a few minutes on an SSD (more on a hard disk): "
                     "the copy is written, then read back whole to check it.",
        "flat_dir": "Folder for the copy", "flat_reading": "Reading the game file…",
        "flat_space_ok": "✓ {free} GB free on this disk: enough for the copy ({need} GB with a margin).",
        "flat_space_low": "✗ Not enough room: this disk has {free} GB free, the copy needs {need} with a margin. Free {more} GB there "
                          "(the recycle bin, your downloads, an old copy of the game you no longer need…), or choose a folder on "
                          "another disk with \"Browse…\", then \"Check the room\".",
        "flat_space_again": "Check the room",
        "flat_done_before": "✓ The copy made before from this same file is there: {path} ({gb} GB). It is taken again: nothing "
                            "to do.",
        "flat_partial": "A copy that was started is there ({done} GB of {gb}): it carries on where it stopped.",
        "flat_discard": "Delete the started copy",
        "flat_other": "✗ This folder already has a game.nup the builder did not make (or it is your dump itself): choose another "
                      "folder.",
        "flat_stale": "An old copy, of another file or another version, is there: it will be replaced.",
        "flat_start": "Make the copy »",
        "flat_made": "✓ The copy is made and checked, in {m} min: {path} ({gb} GB, {n} files unpacked). It is what the console gets.",
        "corrupt_banner": "Corrupted files detected",
        "corrupt_text": "{what} in your dump are not the game's: their fingerprint (in the file itself) is not a sound dump's, the "
                        "dump tool read them wrong.",
        "corrupt_n": "{n} files", "corrupt_1": "One file", "corrupt_missing": " And {m} files of the game are missing.",
        "corrupt_missing_only": "{m} files of the game are missing in your dump: the dump tool did not write them.",
        "corrupt_more": "… and {k} more (all in the log and the report)",
        "corrupt_effect": "Where the game uses one, a texture, a sound or a voice may be missing, or the game may stop. Advice: "
                          "dump your game again (or copy it again from the console). You can go on anyway.",
        "corrupt_kinds": {},
        "corrupt_missing_tag": "missing",
        "corrupt_short": "The list, and what it may do, are on the page before (\"Game\"). Dump the game again if you can, or go "
                         "on anyway.",
        "flat_progress": {},
        "resume": "The game is already built in \"{out}\".",
        "resume_shaders": "Go to the shaders", "resume_console": "Go to the console",
        "p_options": "Step 2 of 6: options",
        "out": "Output folder",
        "out_text": "The folder the title and the data are made in (about 7 GB, the build's work included). A short, plain path is "
                    "safest, C:\\Grapnel for instance.",
        "icons": "Make the controller button icons (from your game files)",
        "advanced": "Advanced +", "advanced_hide": "Advanced –",
        "memory": "Memory for the compile (GB, 0 = automatic)",
        "out_free": "{gb:.0f} GB free on this disk.", "out_resume": " An earlier build is there: it carries on where it stopped.",
        "out_elsewhere": "An earlier build is in \"{other}\": here everything would be done again (about an hour).",
        "out_go_back": "Carry on in that folder",
        "p_check": "Step 3 of 6: check this PC",
        "check_text": "The check runs by itself. Everything must be green to go on.",
        "check_again": "Check again",
        "rows": {"llvm": "Compiler (LLVM 19.1.7)", "toolchain": "The console's tools", "native": "Signing tool",
                 "python": "Python and its packages", "zbic": "Unpacking the game (ZBIC)", "compile": "Test compile for the PS5",
                 "cpu": "Processor", "prepare": "Shader preparation on this PC", "nup": "Game file", "disk": "Disk space",
                 "memory": "Memory", "paths": "Paths"},
        "row_wait": "…", "check_running": "Checking…", "check_ok": "All set.",
        "check_bad": "Something is not right: see the red line.",
        "prep_missing": "missing: the console prepares (step 5)",
        "cpu_ok": "ok",
        "cpu_console": "no AVX2: the console will prepare the shaders at its first start (about an hour on a standard PS5), not "
                       "this PC. Nothing to do: the build sets it up.",
        "prep_cpu": "by the console, at its first start (a processor without AVX2)",
        "disk_ok": "{gb:.0f} GB free", "mem_ok": "{total:.0f} GB ({avail:.0f} GB free)", "mem_low": "{total:.0f} GB: very slow",
        "paths_ok": "short and readable",
        "p_build": "Step 4 of 6: build the game",
        "build_text": "About 30 minutes (up to an hour on a small PC). The PC stays awake; you may minimise the window, but do not "
                      "close it.",
        "chain": "Chain the shaders automatically",
        "left": "about {m} min left", "left_soon": "under a minute", "elapsed": "elapsed: {t}",
        "core_steps": {},
        "build_done": "The game is built. eboot.bin: {bytes} bytes, CRC32 {crc} (0x{crc:08X}).",
        "build_again": "Build again",
        "p_shaders": "Step 5 of 6: build the shaders",
        "shaders_text": "The PC compiles every shader of the game for the console into its driver's cache: the console will have "
                        "nothing to compile in play, it only checks.",
        "shaders_start": "Prepare the shaders »", "shaders_done": "The shaders are ready: {entries} entries in the cache.",
        "shaders_cpu": "This PC does not prepare the shaders: its processor has no AVX2 (x86-64-v3), which the preparation tool "
                       "needs. So the console prepares them itself, at its first start: about an hour on a standard PS5 (on such a "
                       "PC the preparation would take about as long). \"Later\" stops it, and the next start carries on where it "
                       "stopped.\n\nThe build sets it up by itself: the list of the game's shaders (made in step 4 from your game) "
                       "and the setting that offers it go beside the game, in data\\grapnel.",
        "shaders_stopped": "The preparation on this PC stopped on an instruction its processor does not have. So the console "
                           "prepares the shaders itself, at its first start: about an hour on a standard PS5 (\"Later\" stops it, "
                           "and the next start carries on where it stopped). The build set it up: the list of the game's shaders "
                           "and the setting that offers it are in data\\grapnel.\n\nPlease report it with your processor's name "
                           "(Grapnel's issues): this PC should have gone to the console at once.",
        "shaders_no_tool": "The toolchain folder given has no shader preparation for this system (its prepare/ folder).\n\n",
        "shaders_console": "The fallback: the console prepares them itself at the first start, about an hour on a standard PS5 "
                           "(\"Later\" stops it, and it carries on). For that, the button below puts the list of the game's shaders "
                           "(made in step 4 from your game) beside the game, with the setting that offers it. Without it, the game "
                           "compiles the shaders as it plays (stutters).",
        "shaders_console_wait": "The build sets it up at its end.",
        "fallback_btn": "Set up the console's preparation (fallback)",
        "fallback_done": "Ready: copy data\\grapnel with its two more files (step 6). At the first start the console offers \"Prepare "
                         "the shaders\".",
        "fallback_missing": "The shaders' list is missing: build again (step 4).",
        "p_console": "Step 6 of 6: the console",
        "console_intro": "Optional: send the title to the console over the network. Your saves are copied to this PC first. On "
                         "the console, only the title's folder (GRPN00001), the game's folder and /data/grapnel are touched: never "
                         "the other games.",
        "console_place": "Install the title and the game file on:",
        "place_internal": "the internal storage (/data)",
        "place_ext": "the extension disk ({vol})",
        "console_vols": "Extension disk found: {vols}. Its free space is not known (the FTP server does not tell it).",
        "console_vol_none": "No extension disk on the console: the title goes to the internal storage.",
        "console_vol_later": "The extension disk is looked for when sending, once the FTP server runs.",
        "console_title_at": "The title is already on {where}.",
        "console_no_volume": "The extension disk {vol} is not on the console: nothing was sent. Plug it in, or choose the "
                             "internal storage.",
        "console_elsewhere": "The title is also on {old}: once the send is read back, the window offers to remove it.",
        "console_move_title_title": "Remove the old title?",
        "console_move_title": "The title is now on {new}, sent and read back.\n\nAn old copy is still there:\n{old}\n\n"
                              "Remove it? Two GRPN00001 titles would confuse ShadowMountPlus. Your saves are in /data/grapnel: "
                              "they are not in it, and they stay.",
        "console_move_nup_title": "Delete the old copy of the game?",
        "console_move_nup": "The game file is now on {new}, with its size.\n\nAn old copy is still there:\n{old} ({gb:.1f} GB)"
                            "\n\nDelete it? That frees {gb:.1f} GB.",
        "console_removed": "Removed: {what}.",
        "console_kept": "Kept (as you asked): {what}.",
        "console_ip": "Console's IP address", "console_test": "Test the connection",
        "console_testing": "Testing the connection…",
        "console_ftp": "✓ The console answers: its FTP server is ready (port 2121).",
        "console_elfldr": "✓ The console answers (ELF loader, port 9021): sending starts the FTP server ftpsrv there first.",
        "console_no_ftpsrv": "The console answers (ELF loader), but this package has no ftpsrv: start an FTP server on the "
                             "console (port 2121), then test again.",
        "console_none": "Nothing answers at that address (ports 2121 and 9021). Is the console on, jailbroken, on the same "
                        "network, with its ELF loader or an FTP server running?",
        "console_nup": "Send the game file too (game.nup, 26 GB), if it is not there already with its size",
        "console_nup_flat": "Send the game file too: its flat copy (game.nup, {gb} GB), if it is not there already with its size",
        "console_manual_flat": "Your dump keeps files compressed: for the game file, copy the flat copy\n{path}\nnot your dump (the "
                               "console could not read it).",
        "console_send": "Send to the console »", "console_not_built": "Build the game first (step 4).",
        "console_ftpsrv_sent": "The FTP server goes to the console's ELF loader…",
        "console_ftp_late": "The FTP server sent does not answer on port 2121.",
        "console_saving": "Copying your saves to this PC…",
        "console_saved": "Your saves ({n} files) are copied to {dest}.",
        "console_ask_title": "Replace on the console?",
        "console_ask": "The console already has:\n\n{what}\n\nThese folders will be brought up to date with the new build (your "
                       "saves stay there; they are copied to this PC as well{saved}). Go on?",
        "console_ask_saved": ": {dest}",
        "console_rate": "{mb:.1f} MB/s, about {m} min left",
        "console_verifying": "Reading back: the sizes and the eboot's CRC32…",
        "console_ok": "✓ Sent and read back: every file has its size, and the eboot its CRC32 ({crc}).\n\n"
                      "Registering: ShadowMountPlus registers by itself what it finds in {homebrew}, about a minute after "
                      "the send ends; \"Grapnel\" then shows on the home screen. Without ShadowMountPlus, register "
                      "{homebrew}/GRPN00001 with your usual tool.",
        "console_bad": "✗ The read back does not match:\n{what}\n\nSend again: what is right is kept.",
        "console_failed": "✗ The send stopped: {e}\nSending again carries on from there.",
        "console_cancelled": "Stopped. Sending again carries on from there.",
        "console_manual": "Or by hand (FTP or the console's file manager):\n\n"
                        "1. homebrew\\GRPN00001  →  /data/homebrew/GRPN00001, then register the title.\n"
                        "2. data\\grapnel  →  /data/grapnel (if the console already has /data/hookshot: see the notes).\n"
                        "3. Your game file  →  /data/grapnel/game.nup.\n"
                        "On the extension disk: homebrew\\GRPN00001  →  /mnt/ext1/homebrew/GRPN00001, the game file  →  "
                        "/mnt/ext1/grapnel/game.nup, and the line HOOKSHOT_PS5_NUP=/mnt/ext1/grapnel/game.nup in "
                        "/data/grapnel/hookshot_env.txt.\n\n"
                        "These folders come from your game: never share them. All of it is in INSTALL.en.txt.",
        "open_install": "Open the notes", "no_install": "The notes are not there yet: build first.",
        "confirm_cancel": "Stop? What is done is kept: starting again carries on from there.",
        "confirm_quit": "A build is running. Closing the window stops it. Close anyway?",
        "cancelled": "Stopped. Starting again carries on where it was.",
        "warn_memory": "This PC has {total:.0f} GB of memory: 8 at least are needed, 16 to be quick. The build may be very slow. Go "
                       "on?",
        "err_disk": "The output folder's disk has {gb:.1f} GB free; about {need} are needed.",
        "err_path": "The output folder's path is too long for Windows ({n} characters; {max} at most here). Choose a shorter folder, "
                    "C:\\Grapnel for instance.",
        "err_chars": "The path \"{path}\" has characters some tools cannot read on this PC ({chars}). Choose a folder with a plain "
                     "name, C:\\Grapnel for instance.",
        "err_not_dir": "The output folder cannot be made: {e}",
        "err_title": "Grapnel", "err_details": "Details (the whole log: {log}):",
        "crc_pick": "Choose a file (eboot.bin for instance)",
        "crc_result": "{name}\n{bytes} bytes\nCRC32: {crc} (0x{crc:08X})\n\n(copied to the clipboard)",
        "report_copied": "The report is copied: paste it into your message.",
        "worker": "The builder stopped without saying why (code {rc}). The log may tell.",
        "errors": {},
    },
}
for _k, _v in T["fr"]["errors"].items():   # (English: the core's own message, plainly; the French above are the player's words)
    T["en"]["errors"].setdefault(_k, None)


def ui_language():
    """fr when Windows' (or the session's) language is French, else en; GRAPNEL_LANG=fr|en chooses"""
    forced = os.environ.get("GRAPNEL_LANG", "").lower()
    if forced in T:
        return forced
    if WINDOWS:
        try:
            import ctypes
            return "fr" if (ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF) == 0x0C else "en"
        except (OSError, AttributeError):
            pass
    lang = os.environ.get("LC_ALL") or os.environ.get("LC_MESSAGES") or os.environ.get("LANG") or ""
    return "fr" if lang.lower().startswith("fr") else "en"


# ---- this PC ----

def memory_gb():
    """(total, available) in GiB, or (None, None)"""
    if WINDOWS:
        import grapnel_core
        return grapnel_core.windows_memory()
    try:
        info = {}
        with open("/proc/meminfo") as f:
            for line in f:
                k, v = line.split(":", 1)
                info[k] = int(v.split()[0]) / 2 ** 20
        return info.get("MemTotal"), info.get("MemAvailable")
    except (OSError, ValueError):
        return None, None


def long_paths_enabled():
    """Windows' LongPathsEnabled (a system setting: Python can then use paths past 260 characters)"""
    if not WINDOWS:
        return True
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\FileSystem") as k:
            return winreg.QueryValueEx(k, "LongPathsEnabled")[0] == 1
    except OSError:
        return False


def odd_characters(path):
    """The characters of path the system's code page cannot write (a program reading its arguments in it sees '?'), or ''"""
    if not WINDOWS:
        return ""
    import locale
    enc = locale.getencoding() if hasattr(locale, "getencoding") else locale.getpreferredencoding(False)
    bad = []
    for ch in path:
        try:
            ch.encode(enc)
        except (UnicodeEncodeError, LookupError):
            if ch not in bad:
                bad.append(ch)
    return "".join(bad)


def free_gb(folder):
    p = os.path.abspath(folder)
    while p and not os.path.exists(p):
        up = os.path.dirname(p)
        if up == p:
            break
        p = up
    try:
        return shutil.disk_usage(p).free / 2 ** 30
    except OSError:
        return None


def crc32_file(path):
    c = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            c = zlib.crc32(block, c)
    return c


def bundled(args):
    """The builder's own parts: the Windows folder's (python\\, llvm\\, toolchain\\ beside app\\), or the options given"""
    py = os.path.join(TOP, "python", "python.exe")
    if WINDOWS and os.path.isfile(py):
        return {"python": py, "prompt_python": py, "llvm": os.path.join(TOP, "llvm"), "toolchain": os.path.join(TOP, "toolchain")}
    return {"python": args.python or sys.executable, "prompt_python": args.prompt_python or "", "llvm": args.llvm or "",
            "toolchain": args.toolchain or os.environ.get("GRAPNEL_TOOLCHAIN", "")}


def prepare_possible(toolchain):
    """The toolchain has the shaders' preparation for this PC's system (prepare/prepare.json: linux-x64, or windows-x64 since 0.3)"""
    try:
        with open(os.path.join(toolchain, "prepare", "prepare.json"), encoding="utf-8") as f:
            system = json.load(f).get("system", "linux-x64")
    except (OSError, ValueError, TypeError):
        return False
    return system == ("windows-x64" if WINDOWS else "linux-x64")


def this_cpu():
    """grapnel_core.cpu_facts(): whether this PC's processor runs the shaders' preparation (x86-64-v3), what was read. On a
    processor without it the window does as with no preparation tool (the check says so, the build passes --no-prepare) and
    the build sets up the console's own preparation by itself"""
    import grapnel_core
    return grapnel_core.cpu_facts()


def settings_path():
    base = os.environ.get("LOCALAPPDATA") if WINDOWS else os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base or TOP, "Grapnel", "builder-settings.json")


def default_out():
    home = os.path.expanduser("~")
    docs = os.path.join(home, "Documents")
    return os.path.join(docs if os.path.isdir(docs) else home, "Grapnel")


def builder_version():
    """(0.2.4) The builder's release, grapnel_core.VERSION, read from its file without importing it (the window opens first):
    in the window's title and its dialogs' titles, so that a player's screenshot says which version they ran"""
    try:
        with open(os.path.join(HERE, "grapnel_core.py"), encoding="utf-8") as f:
            m = re.search(r'^VERSION = "([^"]+)"', f.read(), re.M)
        return m.group(1) if m else ""
    except OSError:
        return ""


def this_version():
    """This builder's code (app/VERSION, git describe at packaging), "" in a checkout"""
    try:
        with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def has_work(out):
    """An earlier build's work folder is in this output folder (its translation, at least)"""
    return bool(out) and os.path.isdir(os.path.join(out, "grapnel-work", "sa"))


def built(out):
    """What the output folder holds: {"game": eboot's manifest entry or None, "shaders": cache entries or None}. A game another
    builder made (an earlier test package's) is not this one's: the build makes it again, and its shaders with it"""
    res = {"game": None, "shaders": None, "console": None}
    try:
        with open(os.path.join(out, "manifest.json"), encoding="utf-8") as f:
            m = json.load(f)
        mine = this_version()
        if mine and str(m.get("checkout", "")).split(" ")[0] != mine:
            return res
        if os.path.isfile(os.path.join(out, "homebrew", "GRPN00001", "eboot.bin")):
            res["game"] = next((v for k, v in m["files"].items() if k.replace("\\", "/").endswith("/eboot.bin")), None)
        if m.get("prepared") and os.path.isdir(os.path.join(out, "homebrew", "GRPN00001", "radv-shader-cache")):
            res["shaders"] = m["prepared"]
        if m.get("console_prepare"):   # (the console prepares the shaders itself: what the build set up beside the game)
            res["console"] = m["console_prepare"]
    except (OSError, ValueError, KeyError, StopIteration, TypeError):
        pass
    return res


# ---- Windows: the build's processes in a job, the PC kept awake ----

class Job:
    """A job object that holds the build and everything it starts: Cancel ends them all at once, and so does this window's end
    (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE). Linux: the build's process group"""

    def __init__(self):
        self.handle = None
        self.proc = None
        if not WINDOWS:
            return
        import ctypes
        from ctypes import wintypes

        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                        ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]

        class Extended(ctypes.Structure):
            _fields_ = [("Basic", Basic), ("Io", ctypes.c_uint64 * 6), ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]
        k = ctypes.WinDLL("kernel32", use_last_error=True)
        k.CreateJobObjectW.restype = wintypes.HANDLE
        k.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        k.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.k = k
        self.handle = k.CreateJobObjectW(None, None)
        if self.handle:
            info = Extended()
            info.Basic.LimitFlags = 0x2000   # (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)
            k.SetInformationJobObject(self.handle, 9, ctypes.byref(info), ctypes.sizeof(info))

    def start(self, cmd, **kw):
        if WINDOWS:
            kw["creationflags"] = 0x08000000 | 0x00004000   # (no console window; below normal priority, inherited)
        else:
            kw["start_new_session"] = True
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
        if WINDOWS and self.handle:
            self.k.AssignProcessToJobObject(self.handle, int(self.proc._handle))
        return self.proc

    def stop(self):
        if not self.proc or self.proc.poll() is not None:
            return
        if WINDOWS:
            if self.handle:
                self.k.TerminateJobObject(self.handle, 1)
            else:
                subprocess.run(["taskkill", "/T", "/F", "/PID", str(self.proc.pid)], capture_output=True, creationflags=0x08000000)
        else:
            import signal
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except OSError:
                pass


def keep_awake(on):
    if WINDOWS:
        import ctypes
        try:
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | (0x00000001 if on else 0))
        except (OSError, AttributeError):
            pass


def minutes(seconds):
    return max(1, int(seconds / 60 + 0.5))


# ---- the assistant ----

class Assistant:
    def __init__(self, root, args):
        self.root, self.args = root, args
        self.lang = ui_language()
        self.t = dict(T[self.lang])
        version = builder_version()
        if version:   # (0.2.4: "Grapnel builder 0.2.4", "Grapnel 0.2.4" on the dialogs)
            self.t["title"] = "%s %s" % (self.t["title"], version)
            self.t["err_title"] = "%s %s" % (self.t["err_title"], version)
        self.parts = bundled(args)
        # (the PC prepares the shaders when the toolchain has the tool for this system and the processor runs it: x86-64-v3)
        self.cpu = this_cpu()
        self.tool_ok = prepare_possible(self.parts["toolchain"])
        self.can_prepare = self.tool_ok and self.cpu["v3"]
        self.events = queue.Queue()
        self.job = None
        self.running = None          # (kind, start time): kind "check", "build", "shaders"
        self.cancelled = False
        self.session_log, self._pending = [], []
        self.last_error = self.result = None
        self.nup_state = None        # (path, ok, text, facts: grapnel_core.check_nup's, "flat" when its files are compressed)
        self.sub = None              # "flat": the game's page's own next page, the flat copy (a compressed .nup only)
        self.flat_facts = None       # (the copy page's: source, folder, {"size", "state", "free", ...}) as last read
        self.check_state = None      # (inputs, all ok)
        self.done = set()            # the pages passed
        self.page = 0
        self.core_step = 0
        self.eta = self.step_frac = None
        self.check_rows, self.check_problems = {}, []
        self.check_marks = {}        # (each check line's mark: True, None a warning, "info" a note; shown again as it was)
        self.settings = self.load_settings()
        self.add_log("the processor: %s, x86-64-v3 %s (%s)" % (self.cpu.get("name") or "?", "yes" if self.cpu["v3"] else "no",
                                                               self.cpu["read"]))
        root.title(self.t["title"])
        self.set_icon()
        self.layout()
        self.show(0)
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(100, self.pump)

    # ---- settings ----

    def load_settings(self):
        try:
            with open(settings_path(), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return {}

    def save_settings(self):
        out = self.out_var.get()
        # (the output folders used, newest first: an earlier build elsewhere is offered back on the options page)
        outs = [out] + [o for o in self.settings.get("outs", []) if o != out] if out else self.settings.get("outs", [])
        self.settings.update(nup=self.nup_var.get(), out=out, icons=bool(self.icons_var.get()),
                             memory=self.memory_var.get(), chain=bool(self.chain_var.get()), outs=outs[:6],
                             console_ip=self.ip_var.get().strip(), send_nup=bool(self.send_nup_var.get()),
                             console_place=self.place_var.get(), flat_dir=self.flat_dir_var.get().strip())
        try:
            os.makedirs(os.path.dirname(settings_path()), exist_ok=True)
            with open(settings_path(), "w", encoding="utf-8") as f:
                json.dump(self.settings, f, indent=1)
        except OSError:
            pass

    def set_icon(self):
        ico, png = os.path.join(HERE, "window", "grapnel.ico"), os.path.join(HERE, "window", "grapnel.png")
        try:
            if WINDOWS and os.path.isfile(ico):
                self.root.iconbitmap(default=ico)
            elif os.path.isfile(png):
                self.root.iconphoto(True, tk.PhotoImage(file=png))
        except tk.TclError:
            pass

    # ---- the frame: steps on top, a page, the buttons below ----

    def layout(self):
        t, root = self.t, self.root
        root.minsize(820, 560)
        big = ("Segoe UI", 16, "bold") if WINDOWS else ("TkDefaultFont", 16, "bold")
        self.bold = ("Segoe UI", 11, "bold") if WINDOWS else ("TkDefaultFont", 11, "bold")
        head = ttk.Frame(root, padding=(12, 10, 12, 4))
        head.pack(fill="x")
        try:
            self.logo = tk.PhotoImage(file=os.path.join(HERE, "window", "grapnel.png")).subsample(9)
            ttk.Label(head, image=self.logo).pack(side="left", padx=(0, 10))
        except tk.TclError:
            pass
        ttk.Label(head, text="Grapnel", font=big).pack(side="left", padx=(0, 16))
        self.step_labels = []
        steps = ttk.Frame(head)
        steps.pack(side="left", fill="x", expand=True)
        for i, name in enumerate(t["steps"]):
            if i:
                ttk.Label(steps, text="─", foreground="#999").pack(side="left", padx=2)
            lab = ttk.Label(steps, text="", foreground="#888")
            lab.pack(side="left")
            self.step_labels.append((lab, name))
        self.back_btn = ttk.Button(head, text=t["back"], command=self.back)
        self.back_btn.pack(side="right")
        ttk.Separator(root).pack(fill="x")
        self.body = ttk.Frame(root, padding=(18, 12))
        self.body.pack(fill="both", expand=True)
        self.logf = ttk.Frame(root, padding=(12, 0))
        self.log = tk.Text(self.logf, height=12, wrap="none", state="disabled", font=("Consolas", 9) if WINDOWS else ("TkFixedFont", 9))
        ys = ttk.Scrollbar(self.logf, orient="vertical", command=self.log.yview)
        self.log.configure(yscrollcommand=ys.set)
        self.log.pack(side="left", fill="both", expand=True)
        ys.pack(side="right", fill="y")
        self.log_visible = False
        ttk.Separator(root).pack(fill="x")
        foot = ttk.Frame(root, padding=(12, 8))
        foot.pack(fill="x", side="bottom")
        self.foot = foot
        ttk.Button(foot, text=t["open_out"], command=self.open_out).pack(side="left")
        ttk.Button(foot, text=t["copy_report"], command=self.copy_report).pack(side="left", padx=4)
        self.log_btn = ttk.Button(foot, text=t["log_show"], command=self.toggle_log)
        self.log_btn.pack(side="left")
        ttk.Button(foot, text=t["crc_tool"], command=self.crc_tool).pack(side="left", padx=4)
        self.next_btn = ttk.Button(foot, text=t["next"], command=self.next)
        self.next_btn.pack(side="right")
        self.cancel_btn = ttk.Button(foot, text=t["cancel"], command=self.cancel, state="disabled")
        self.cancel_btn.pack(side="right", padx=6)
        # (the pages' variables, kept across pages)
        self.nup_var = tk.StringVar(value=self.settings.get("nup", ""))
        self.out_var = tk.StringVar(value=self.settings.get("out") or default_out())
        self.icons_var = tk.IntVar(value=1 if self.settings.get("icons", True) else 0)
        self.memory_var = tk.StringVar(value=str(self.settings.get("memory", "0")))
        self.chain_var = tk.IntVar(value=1 if self.settings.get("chain", True) else 0)
        self.ip_var = tk.StringVar(value=self.settings.get("console_ip", ""))
        self.flat_dir_var = tk.StringVar(value=self.settings.get("flat_dir", ""))
        self.send_nup_var = tk.IntVar(value=1 if self.settings.get("send_nup", True) else 0)
        # (where step 6 installs: the internal storage or an extension disk, /mnt/ext1; the title's place once the console says)
        chosen = getattr(self.args, "place", None)   # (--place, tests: as if chosen on the page)
        self.place_var = tk.StringVar(value=chosen or self.settings.get("console_place", grapnel_send.INTERNAL))
        self.place_touched = bool(chosen)
        self.console_found = None
        self.nup_var.trace_add("write", lambda *a: self.root.after(300, self.nup_changed))

    def toggle_log(self):
        self.log_visible = not self.log_visible
        if self.log_visible:
            self.logf.pack(fill="both", expand=True, before=self.foot)
            self.log_btn.config(text=self.t["log_hide"])
        else:
            self.logf.pack_forget()
            self.log_btn.config(text=self.t["log_show"])

    def paint_steps(self):
        for i, (lab, name) in enumerate(self.step_labels):
            if i == 0 and self.sub == "flat":
                name = "%s ▸ %s" % (name, self.t["step_flat"])
            mark = "✓ " if i in self.done and i != self.page else ("● " if i == self.page else "")
            lab.config(text="%s%d %s" % (mark, i + 1, name), foreground="#1a7f37" if i in self.done and i != self.page else
                       ("#000" if i == self.page else "#888"), font=self.bold if i == self.page else "")

    def show(self, page, sub=None):
        self.page, self.sub = page, sub
        for w in self.body.winfo_children():
            w.destroy()
        getattr(self, "page_" + (sub or PAGES[page]))()
        self.paint_steps()
        self.back_btn.config(state="normal" if (page > 0 or sub) and not self.running else "disabled")
        self.update_next()

    def heading(self, title, text=""):
        ttk.Label(self.body, text=title, font=self.bold).pack(anchor="w")
        if text:
            ttk.Label(self.body, text=text, wraplength=760, justify="left").pack(anchor="w", pady=(4, 10))

    def update_next(self):
        t, p = self.t, PAGES[self.page]
        ok = not self.running
        label = t["next"]
        if self.sub == "flat":
            ok = ok and self.flat_ready()
        elif p == "game":
            ok = ok and bool(self.nup_state and self.nup_state[1] and self.nup_state[0] == self.nup_var.get().strip())
        elif p == "options":
            ok = ok and bool(self.out_var.get().strip())
        elif p == "check":
            ok = ok and bool(self.check_state and self.check_state[1] and self.check_state[0] == self.inputs())
            label = t["build_btn"]
        elif p == "build":
            ok = ok and built(self.out()).get("game") is not None
        elif p == "console":
            label = t["finish"]
        self.next_btn.config(text=label, state="normal" if ok else "disabled")

    def next(self):
        if self.page == len(PAGES) - 1:
            return self.on_close()
        if PAGES[self.page] == "options" and not self.options_ok():
            return
        self.save_settings()
        if PAGES[self.page] == "game" and self.sub is None and self.needs_flat():
            return self.show(0, "flat")   # (a .nup whose game files are compressed: its flat copy first)
        self.done.add(self.page)
        self.show(self.page + 1)

    def back(self):
        if self.running:
            return
        if self.sub == "flat":
            self.show(0)
        elif PAGES[self.page] == "options" and self.needs_flat():
            self.show(0, "flat")
        elif self.page > 0:
            self.show(self.page - 1)

    def out(self):
        return os.path.abspath(self.out_var.get().strip()) if self.out_var.get().strip() else ""

    def inputs(self):
        return (self.nup_var.get().strip(), self.out(), bool(self.icons_var.get()))

    # ---- page 1: the game ----

    def page_game(self):
        t = self.t
        self.heading(t["p_game"], t["p_game_text"])
        row = ttk.Frame(self.body)
        row.pack(fill="x")
        ttk.Label(row, text=t["nup"]).pack(side="left")
        ttk.Entry(row, textvariable=self.nup_var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(row, text=t["browse"], command=self.pick_nup).pack(side="left")
        self.nup_note = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.nup_note.pack(anchor="w", pady=(8, 0))
        self.nup_damage = ttk.Frame(self.body)   # (corrupt_box: the dump's corrupted or missing files, when it has some)
        self.paint_nup()
        b = built(self.out())
        if b["game"]:
            box = ttk.Frame(self.body, padding=(0, 18, 0, 0))
            box.pack(anchor="w")
            ttk.Label(box, text=t["resume"].format(out=self.out())).pack(anchor="w")
            ttk.Button(box, text=t["resume_shaders"], command=lambda: self.jump(4)).pack(side="left", pady=6)
            ttk.Button(box, text=t["resume_console"], command=lambda: self.jump(5)).pack(side="left", padx=6, pady=6)
        if self.nup_var.get().strip() and not self.nup_state:
            self.nup_changed()

    def jump(self, page):
        self.done.update(range(page))
        self.show(page)

    def paint_nup(self):
        if not hasattr(self, "nup_note") or not self.nup_note.winfo_exists():
            return
        path = self.nup_var.get().strip()
        if not path:
            self.nup_note.config(text=self.t["nup_none"], foreground="#555")
        elif not self.nup_state or self.nup_state[0] != path:
            self.nup_note.config(text=self.t["nup_checking"], foreground="#555")
        else:
            self.nup_note.config(text=self.nup_state[2], foreground="#1a7f37" if self.nup_state[1] else "#b42318")
        found = self.nup_state[3].get("game_files") if self.nup_state and self.nup_state[0] == path else None
        self.corrupt_box(self.nup_damage, (found or {}).get("corrupted") or [], (found or {}).get("missing") or [])

    def corrupt_box(self, holder, corrupted, missing, shown=8):
        """(the player's wish, 25/11/2026) A dump's corrupted and missing files, as plainly as "Compression detected": in a red
        frame, "Corrupted files detected" in bold, how many, which (name and what it is), what it may do in the game, the
        advice; Next stays possible. holder: a frame of the page, emptied first; nothing in it for a sound dump. shown: the files
        named at most (0: the compression's page, short, which sends to the game's page for the list)"""
        if holder is None or not holder.winfo_exists():
            return
        for w in holder.winfo_children():
            w.destroy()
        if not corrupted and not missing:
            if holder.winfo_manager():
                holder.pack_forget()
            return
        t = self.t
        import grapnel_game_files as gf
        big = ("Segoe UI", 13, "bold") if WINDOWS else ("TkDefaultFont", 13, "bold")
        box = tk.Frame(holder, highlightthickness=2, highlightbackground="#b42318", highlightcolor="#b42318", padx=10, pady=6)
        box.pack(fill="x")
        tk.Label(box, text="⚠ " + t["corrupt_banner"], font=big, fg="#b42318").pack(anchor="w")
        if corrupted:
            what = t["corrupt_1"] if len(corrupted) == 1 else t["corrupt_n"].format(n=self.count(len(corrupted)))
            text = t["corrupt_text"].format(what=what) + (t["corrupt_missing"].format(m=self.count(len(missing))) if missing else "")
        else:
            text = t["corrupt_missing_only"].format(m=self.count(len(missing)))
        if not shown:   # (the compression's page: short, the game's page has the list)
            tk.Label(box, text=text + " " + t["corrupt_short"], wraplength=740, justify="left").pack(anchor="w")
            if not holder.winfo_manager():
                holder.pack(fill="x", pady=(0, 8), after=self.flat_banner)
            return
        tk.Label(box, text=text, wraplength=740, justify="left").pack(anchor="w")
        kinds = t["corrupt_kinds"] or gf.KIND_EN
        every = [(f, None) for f in corrupted] + [(f, t["corrupt_missing_tag"]) for f in missing]
        shown = shown if len(every) > shown + 1 else len(every)   # ("and 1 more" would take the line the file takes)
        rows = ["• %s (%s%s)" % (f.rsplit("/", 1)[-1], kinds[gf.kind_of(f)], ", " + tag if tag else "") for f, tag in every[:shown]]
        if len(every) > shown:
            rows.append(t["corrupt_more"].format(k=self.count(len(every) - shown)))
        tk.Label(box, text="\n".join(rows), justify="left", font=("Consolas", 9) if WINDOWS else ("TkFixedFont", 9)).pack(anchor="w", pady=(2, 2))
        tk.Label(box, text=t["corrupt_effect"], wraplength=740, justify="left", fg="#7a271a").pack(anchor="w")
        if not holder.winfo_manager():
            holder.pack(fill="x", pady=(8, 0), after=self.nup_note if holder is getattr(self, "nup_damage", None) else self.flat_banner)

    def pick_nup(self):
        start = os.path.dirname(self.nup_var.get()) if self.nup_var.get() else os.path.expanduser("~")
        p = filedialog.askopenfilename(title=self.t["nup"], initialdir=start, filetypes=[("Nintendo .nup", "*.nup"), ("*", "*")])
        if p:
            self.nup_var.set(os.path.normpath(p))

    def nup_changed(self):
        path = self.nup_var.get().strip()
        if self.nup_state and self.nup_state[0] == path:
            return
        self.nup_state = None
        self.paint_nup()
        self.update_next()
        if not path:
            return

        def work():
            try:
                import grapnel_core
                facts = {}
                grapnel_core.check_nup(path, facts)
                text = self.t["nup_ok"] + (self.t["nup_flat"].format(what=self.flat_what(facts)) if facts.get("flat") else "")
                self.events.put(("nup", path, True, text, facts))
            except Exception as e:
                text = self.t["errors"].get(getattr(e, "code", None)) or str(e)
                self.events.put(("nup", path, False, "✗ " + text, {}))
        threading.Thread(target=work, daemon=True).start()

    def on_nup(self, path, ok, text, facts=None):
        if path == self.nup_var.get().strip():
            self.nup_state = (path, ok, text, facts or {})
            self.paint_nup()
            self.update_next()

    # ---- the flat copy (a .nup whose game files are compressed: its own page after the game's) ----

    def needs_flat(self):
        st = self.nup_state
        return bool(st and st[1] and st[0] == self.nup_var.get().strip() and st[3].get("flat"))

    def flat_what(self, facts):
        if facts.get("coded"):
            return self.t["what_coded"].format(coded=self.count(facts["coded"]), files=self.count(facts["files"]))
        return self.t["what_framed"]

    def count(self, n):
        """n with its thousands apart: 40,588 (40 588 in French, a narrow space)"""
        return "{:,}".format(n).replace(",", "\u202f" if self.lang == "fr" else ",")

    def gb(self, n):
        """n bytes in GB with one decimal, a comma in French"""
        text = "%.1f" % (n / 1e9)
        return text.replace(".", ",") if self.lang == "fr" else text

    def flat_dir(self):
        d = self.flat_dir_var.get().strip() or self.out() or default_out()
        return os.path.abspath(d)

    def flat_ready(self):
        """The copy for this .nup, in the copy's folder, is made and checked (or one made before is there whole)"""
        f = self.flat_facts
        return bool(f and f[0] == self.nup_var.get().strip() and f[1] == self.flat_dir() and f[2].get("state") == "done")

    def page_flat(self):
        t = self.t
        facts = self.nup_state[3] if self.nup_state else {}
        # (the player's wish, 25/11/2026: said first, in bold and large, before the explanation, the room and the button)
        big = ("Segoe UI", 15, "bold") if WINDOWS else ("TkDefaultFont", 15, "bold")
        self.flat_banner = ttk.Label(self.body, text="⚠ " + t["flat_banner"], font=big, foreground="#b45309")
        self.flat_banner.pack(anchor="w", pady=(0, 8))
        self.flat_damage = ttk.Frame(self.body)   # (corrupt_box, short: the game's page has the longer list)
        found = facts.get("game_files") or {}
        self.corrupt_box(self.flat_damage, found.get("corrupted") or [], found.get("missing") or [], shown=0)
        self.heading(t["p_flat"], t["flat_text"].format(what=self.flat_what(facts)))
        self.flat_cost = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.flat_cost.pack(anchor="w", pady=(0, 8))
        if not self.flat_dir_var.get().strip():
            self.flat_dir_var.set(self.flat_dir())
        row = ttk.Frame(self.body)
        row.pack(fill="x")
        ttk.Label(row, text=t["flat_dir"]).pack(side="left")
        ttk.Entry(row, textvariable=self.flat_dir_var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(row, text=t["browse"], command=self.pick_flat_dir).pack(side="left")
        self.flat_note = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.flat_note.pack(anchor="w", pady=(6, 0))
        buttons = ttk.Frame(self.body)
        buttons.pack(anchor="w", pady=8)
        self.flat_btn = ttk.Button(buttons, text=t["flat_start"], command=self.run_flatten, state="disabled")
        self.flat_btn.pack(side="left")
        ttk.Button(buttons, text=t["flat_space_again"], command=self.read_flat).pack(side="left", padx=6)
        self.flat_discard_btn = ttk.Button(buttons, text=t["flat_discard"], command=self.discard_flat)
        self.progress_page()
        if not getattr(self, "flat_trace", None):
            self.flat_trace = self.flat_dir_var.trace_add("write", lambda *a: self.root.after(400, self.read_flat))
        if self.running and self.running[0] == "flatten":
            return
        self.read_flat()

    def pick_flat_dir(self):
        p = filedialog.askdirectory(title=self.t["flat_dir"], initialdir=self.flat_dir(), mustexist=False)
        if p:
            self.flat_dir_var.set(os.path.normpath(p))

    def read_flat(self):
        """What the copy's folder holds for this .nup, the room on its disk, the copy's size (read in a thread: the .nup's TOC)"""
        if not hasattr(self, "flat_note") or not self.flat_note.winfo_exists() or self.sub != "flat":
            return
        src, folder = self.nup_var.get().strip(), self.flat_dir()
        self.flat_note.config(text=self.t["flat_reading"], foreground="#555")

        def work():
            import grapnel_flat
            facts = {}
            try:
                st = grapnel_flat.state(src, folder)
                p, _, lay = grapnel_flat.plan(src)
                p.f.close()
                facts = {"state": st["kind"], "size": lay.size, "written": st["written"], "dst": st["dst"],
                         "need": grapnel_flat.need_bytes(lay, st["written"] if st["kind"] == "partial" else 0),
                         "free": grapnel_flat.free_bytes(folder), "mark": st["mark"]}
                if st["kind"] == "done":
                    why = grapnel_flat.reuse_check(st["dst"], st["mark"], lay.toc_sha256)
                    facts["state"] = "done" if not why else "stale"
            except Exception as e:   # (the folder cannot be read, the .nup is gone...)
                facts = {"state": "error", "error": str(e)}
            self.events.put(("flat_facts", src, folder, facts))
        threading.Thread(target=work, daemon=True).start()

    def on_flat_facts(self, src, folder, facts):
        self.flat_facts = (src, folder, facts)
        self.paint_flat()

    def paint_flat(self):
        if not hasattr(self, "flat_note") or not self.flat_note.winfo_exists() or not self.flat_facts:
            return
        t = self.t
        src, folder, f = self.flat_facts
        if src != self.nup_var.get().strip() or folder != self.flat_dir():
            return
        state = f.get("state")
        if state == "error":
            self.flat_note.config(text="✗ " + f.get("error", ""), foreground="#b42318")
            self.flat_btn.config(state="disabled")
            return self.update_next()
        gb = self.gb(f["size"])
        self.flat_cost.config(text=t["flat_cost"].format(gb=gb))
        lines, colour, can = [], "#1a7f37", True
        made = getattr(self, "flat_made", None)
        if state == "done" and made and made.get("path") == f["dst"]:   # (made just now, on this page)
            lines.append(t["flat_made"].format(path=f["dst"], gb=gb, m=minutes(made.get("seconds") or 0),
                                               n=made.get("unpacked_codec1", 0) + made.get("unpacked_frames", 0)))
            can = False
        elif state == "done":
            lines.append(t["flat_done_before"].format(path=f["dst"], gb=gb))
            can = False
        elif state == "other":
            lines.append(t["flat_other"])
            colour, can = "#b42318", False
        else:
            if state == "partial":
                lines.append(t["flat_partial"].format(done=self.gb(f["written"]), gb=gb))
            elif state == "stale":
                lines.append(t["flat_stale"])
            need, free = f["need"], f["free"]
            if free >= need:
                lines.append(t["flat_space_ok"].format(free=self.gb(free), need=self.gb(need)))
            else:
                lines.append(t["flat_space_low"].format(free=self.gb(free), need=self.gb(need), more=self.gb(need - free + 5e7)))
                colour, can = "#b42318", False
            if state != "partial" and colour == "#1a7f37":
                colour = "#333"
        if state == "done":   # (what the copy read: a .zs that does not start as zstd, a file unlike its record, with the TOC's)
            mark = f.get("mark") or {}
            found = (self.nup_state[3].get("game_files") if self.nup_state else None) or {}
            corrupted = sorted(set(found.get("corrupted") or []) | set(mark.get("not_zstd") or []) |
                               set((mark.get("damaged") or {}).keys()))   # (the same paths: romfs/..., exefs/...)
            self.corrupt_box(self.flat_damage, corrupted, found.get("missing") or [], shown=0)
        if state == "done" and not self.running:   # (nothing to show a bar for: the page's room for the lines above)
            for w in (self.step_text, self.bar, self.progress_row):
                w.pack_forget()
        elif not self.bar.winfo_manager():   # (another folder: the bar back, where it was)
            self.step_text.pack(anchor="w", pady=(6, 2), before=self.result_text)
            self.bar.pack(fill="x", pady=4, before=self.result_text)
            self.progress_row.pack(fill="x", before=self.result_text)
        self.flat_note.config(text="\n".join(lines), foreground=colour)
        self.flat_btn.config(state="normal" if can and not self.running else "disabled")
        if state == "partial" and not self.running:
            self.flat_discard_btn.pack(side="left")
        else:
            self.flat_discard_btn.pack_forget()
        if state == "done" and hasattr(self, "bar") and self.bar.winfo_exists():
            self.bar.config(value=1000)
        self.update_next()

    def discard_flat(self):
        import grapnel_flat
        if self.running or not self.flat_facts:
            return
        _, part, mark = grapnel_flat.paths_for(self.flat_dir())
        for f in (part, mark):
            try:
                os.remove(f)
            except OSError:
                pass
        self.read_flat()

    def run_flatten(self):
        if self.running:
            return
        self.save_settings()
        self.flat_btn.config(state="disabled")
        self.flat_discard_btn.pack_forget()
        self.result_text.config(text="")
        self.bar.config(mode="determinate", value=0)   # (a new copy starts from nothing, whatever the bar showed)
        self.step_text.config(text="")
        self.start("flatten", self.command("flatten"))

    # ---- page 2: the options ----

    def page_options(self):
        t = self.t
        self.heading(t["p_options"], t["out_text"])
        row = ttk.Frame(self.body)
        row.pack(fill="x")
        ttk.Label(row, text=t["out"]).pack(side="left")
        ttk.Entry(row, textvariable=self.out_var).pack(side="left", fill="x", expand=True, padx=8)
        ttk.Button(row, text=t["browse"], command=self.pick_out).pack(side="left")
        self.out_note = ttk.Label(self.body, text="", foreground="#555", wraplength=760)
        self.out_note.pack(anchor="w", pady=(6, 0))
        # (an earlier build in another output folder: said, with the way back to it; all is done again in a new folder)
        self.elsewhere = ttk.Frame(self.body)
        self.elsewhere.pack(anchor="w", fill="x", pady=(4, 0))
        ttk.Frame(self.body).pack(pady=(0, 8))
        ttk.Checkbutton(self.body, text=t["icons"], variable=self.icons_var).pack(anchor="w")
        adv = ttk.Frame(self.body, padding=(0, 14, 0, 0))
        adv.pack(anchor="w", fill="x")
        inner = ttk.Frame(adv, padding=(16, 6, 0, 0))
        ttk.Label(inner, text=t["memory"]).pack(side="left")
        ttk.Spinbox(inner, from_=0, to=256, increment=1, width=6, textvariable=self.memory_var).pack(side="left", padx=6)

        def toggle():
            if inner.winfo_ismapped():
                inner.pack_forget()
                btn.config(text=t["advanced"])
            else:
                inner.pack(anchor="w")
                btn.config(text=t["advanced_hide"])
        btn = ttk.Button(adv, text=t["advanced"], command=toggle)
        btn.pack(anchor="w")
        self.out_var.trace_add("write", lambda *a: self.root.after(300, self.paint_out))
        self.paint_out()

    def paint_out(self):
        if not hasattr(self, "out_note") or not self.out_note.winfo_exists():
            return
        free = free_gb(self.out() or default_out())
        text = self.t["out_free"].format(gb=free) if free is not None else ""
        here = has_work(self.out())
        if here:
            text += self.t["out_resume"]
        self.out_note.config(text=text)
        for w in self.elsewhere.winfo_children() if hasattr(self, "elsewhere") and self.elsewhere.winfo_exists() else ():
            w.destroy()
        known = [self.settings.get("out")] + self.settings.get("outs", [])   # (an older package's settings have "out" alone)
        other = None if here else next((o for o in known if o and os.path.normpath(o) != os.path.normpath(self.out()) and has_work(o)),
                                       None)
        if other:
            ttk.Label(self.elsewhere, text=self.t["out_elsewhere"].format(other=other), foreground="#9a6700",
                      wraplength=760, justify="left").pack(anchor="w")
            ttk.Button(self.elsewhere, text=self.t["out_go_back"], command=lambda: self.out_var.set(other)).pack(anchor="w", pady=4)
        self.update_next()

    def pick_out(self):
        p = filedialog.askdirectory(title=self.t["out"], initialdir=self.out() or os.path.expanduser("~"), mustexist=False)
        if p:
            self.out_var.set(os.path.normpath(p))

    def options_ok(self):
        t, out = self.t, self.out()
        bad = odd_characters(out)
        if bad:
            messagebox.showerror(t["err_title"], t["err_chars"].format(path=out, chars=" ".join(bad)))
            return False
        limit = 259 - DEEPEST - len("\\grapnel-work\\")
        if WINDOWS and not long_paths_enabled() and len(out) > limit:
            messagebox.showerror(t["err_title"], t["err_path"].format(n=len(out), max=limit))
            return False
        try:
            os.makedirs(out, exist_ok=True)
        except OSError as e:
            messagebox.showerror(t["err_title"], t["err_not_dir"].format(e=e))
            return False
        return True

    # ---- page 3: the check ----

    ROWS = ("llvm", "toolchain", "native", "python", "zbic", "compile", "cpu", "prepare", "nup", "disk", "memory", "paths")
    INFO = (("ok: LLVM", "llvm"), ("ok: toolchain", "toolchain"), ("ok: ps5-native-tool", "native"), ("ok: Python", "python"),
            ("ok: the ZBIC program", "zbic"), ("ok: clang", "compile"), ("ok: prospero-clang", "compile"),
            ("ok: the shader preparation", "prepare"), ("ok: the .nup", "nup"))
    CODE_ROW = {"llvm": "llvm", "toolchain": "toolchain", "python": "python", "python-icons": "python", "zbic": "zbic",
                "nup-folder": "nup", "nup-missing": "nup", "nup-not-nup": "nup", "nup-damaged": "nup", "nup-wrong-game": "nup", "nup-codec": "nup", "prepare-cpu": "prepare",
                "prepare-tool": "prepare", "prepare-platform": "prepare", "prepare-stale": "prepare"}

    def page_check(self):
        t = self.t
        self.heading(t["p_check"], t["check_text"])
        grid = ttk.Frame(self.body)
        grid.pack(anchor="w", fill="x")
        self.rows = {}
        for i, key in enumerate(self.ROWS):
            mark = ttk.Label(grid, text="·", width=2, font=self.bold)
            mark.grid(row=i, column=0, sticky="w")
            ttk.Label(grid, text=t["rows"][key]).grid(row=i, column=1, sticky="w", padx=(4, 16))
            note = ttk.Label(grid, text="", foreground="#555", wraplength=520, justify="left")
            note.grid(row=i, column=2, sticky="w")
            self.rows[key] = (mark, note)
        self.check_note = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.check_note.pack(anchor="w", pady=(12, 4))
        ttk.Button(self.body, text=t["check_again"], command=self.run_check).pack(anchor="w")
        if self.check_state and self.check_state[0] == self.inputs() and self.check_state[1]:
            for key in self.ROWS:
                self.set_row(key, self.check_marks.get(key, True), self.check_rows.get(key, ""))
            self.check_note.config(text=t["check_ok"], foreground="#1a7f37")
        else:
            self.root.after(200, self.run_check)

    @staticmethod
    def summary(row, text):
        """A few words for a check's line, from the core's own (English) line"""
        pats = {"llvm": r"version (19\.\d+\.\d+)", "toolchain": r"toolchain (\S+ [\d.]+).*?(\d+) files",
                "nup": r"holds (\S+ v\d+)", "prepare": r"shader stamp (\w+)"}
        m = re.search(pats.get(row, "$^"), text)
        if row == "llvm" and m:
            return "clang " + m.group(1)
        if row == "toolchain" and m:
            return "%s, %s ✓" % (m.group(1), m.group(2))
        if row in ("nup", "prepare") and m:
            return m.group(1)
        if row == "python":
            return " ".join(w for w in text.split(":")[-1].split() if not os.sep in w)[:60]
        return "ok"

    def set_row(self, key, ok, note=""):
        if not hasattr(self, "rows") or key not in self.rows or not self.rows[key][0].winfo_exists():
            return
        mark, lab = self.rows[key]
        # (ok: True a pass, False a problem, None a warning, "info" a note that is no problem: the check stays green)
        mark.config(text={True: "✓", False: "✗", None: "!", "info": "i"}[ok],
                    foreground={True: "#1a7f37", False: "#b42318", None: "#b26a00", "info": "#1f5fa8"}[ok])
        lab.config(text=note)
        if ok is not False:
            self.check_rows[key] = note
            self.check_marks[key] = ok

    def run_check(self):
        if self.running:
            return
        t = self.t
        self.check_rows, self.check_marks = {}, {}
        for key in self.ROWS:
            self.set_row(key, None, t["row_wait"])
        self.check_note.config(text=t["check_running"], foreground="#555")
        problems = []
        # (this PC: what the window checks itself)
        out = self.out()
        free = free_gb(out)
        work_there = os.path.isdir(os.path.join(out, "grapnel-work", "sa"))
        if free is None or free >= (5 if work_there else DISK_GB):
            self.set_row("disk", True, t["disk_ok"].format(gb=free or 0))
        else:
            self.set_row("disk", False, t["err_disk"].format(gb=free, need=DISK_GB))
            problems.append("disk")
        total, avail = memory_gb()
        if total and total < MEMORY_GB:
            self.set_row("memory", None, t["mem_low"].format(total=total))
        else:
            self.set_row("memory", True, t["mem_ok"].format(total=total or 0, avail=avail or 0))
        bad = [p for p in (self.nup_var.get().strip(), out) if odd_characters(p)]
        if bad:
            self.set_row("paths", False, t["err_chars"].format(path=bad[0], chars=odd_characters(bad[0])))
            problems.append("paths")
        else:
            self.set_row("paths", True, t["paths_ok"])
        # (the processor: without x86-64-v3 a note, not a problem; the console prepares the shaders, which the build sets up)
        if self.cpu["v3"]:
            self.set_row("cpu", True, t["cpu_ok"])
        else:
            self.set_row("cpu", "info", t["cpu_console"])
            self.set_row("prepare", "info", t["prep_cpu"])
        if self.cpu["v3"] and not self.can_prepare:
            self.set_row("prepare", None, t["prep_missing"])
        self.check_problems = problems
        self.start("check", self.command("check"))

    # ---- pages 4 and 5: the game, the shaders ----

    def page_build(self):
        t = self.t
        self.heading(t["p_build"], t["build_text"])
        self.progress_page(chain=True)
        b = built(self.out())
        if self.running and self.running[0] in ("build", "shaders"):
            return
        if b["game"]:
            self.show_built(b)
            ttk.Button(self.body, text=t["build_again"], command=lambda: self.run_build(False)).pack(anchor="w", pady=8)
        else:
            self.root.after(300, lambda: self.run_build(self.can_prepare and bool(self.chain_var.get())))

    def progress_page(self, chain=False):
        self.step_text = ttk.Label(self.body, text="", font=self.bold)
        self.step_text.pack(anchor="w", pady=(6, 2))
        self.bar = ttk.Progressbar(self.body, mode="determinate", maximum=1000)
        self.bar.pack(fill="x", pady=4)
        row = self.progress_row = ttk.Frame(self.body)
        row.pack(fill="x")
        self.left_text = ttk.Label(row, text="")
        self.left_text.pack(side="left")
        self.time_text = ttk.Label(row, text="", foreground="#555")
        self.time_text.pack(side="right")
        if chain and self.can_prepare:   # (a PC that does not prepare: nothing to chain, the shaders' page says why)
            ttk.Checkbutton(self.body, text=self.t["chain"], variable=self.chain_var).pack(anchor="w", pady=(10, 0))
        self.result_text = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.result_text.pack(anchor="w", pady=(12, 0))

    def show_built(self, b):
        if b.get("game") and hasattr(self, "result_text") and self.result_text.winfo_exists():
            self.bar.config(value=1000)
            self.result_text.config(text=self.t["build_done"].format(bytes=b["game"]["bytes"], crc=b["game"]["crc32"]),
                                    foreground="#1a7f37")

    def page_shaders(self):
        t = self.t
        b = built(self.out())
        # (the PC does not prepare: its processor, or no tool; or it did, and its run stopped on an instruction the processor does
        # not have, which the build turned into the console's preparation. An older build's console preparation made when the
        # processor was taken for one without x86-64-v3 (GRAPNEL_CPU_V3=0) is not that: this PC prepares)
        if not self.can_prepare or (b.get("console") or {}).get("why") == "illegal-instruction":
            return self.page_shaders_console(b)
        self.heading(t["p_shaders"], t["shaders_text"])
        self.progress_page()
        if self.running and self.running[0] in ("build", "shaders"):
            return
        if b["shaders"]:
            self.bar.config(value=1000)
            self.result_text.config(text=t["shaders_done"].format(entries=b["shaders"].get("entries")), foreground="#1a7f37")
        else:
            self.shaders_btn = ttk.Button(self.body, text=t["shaders_start"], command=self.run_shaders)
            self.shaders_btn.pack(anchor="w", pady=8)

    def page_shaders_console(self, b):
        """The shaders' page when this PC does not prepare them: the console's own preparation, at its first start. Its
        processor has no x86-64-v3 (the build set it up by itself), or the PC's preparation stopped on an instruction the
        processor does not have (the build switched to it), or the toolchain has no preparation for this system (the
        fallback button sets it up: grapnel_core.setup_console_prepare, the build's own)"""
        t = self.t
        why = (b.get("console") or {}).get("why") or ("cpu" if not self.cpu["v3"] else "tool")
        text = {"cpu": t["shaders_cpu"], "illegal-instruction": t["shaders_stopped"]}.get(why) or \
            t["shaders_no_tool"] + t["shaders_console"]
        self.heading(t["p_shaders"])
        ttk.Label(self.body, text=text, wraplength=760, justify="left").pack(anchor="w", pady=(4, 10))
        building = bool(self.running and self.running[0] in ("build", "shaders"))
        ready = self.fallback_ready()
        if not ready and not building:
            ttk.Button(self.body, text=t["fallback_btn"], command=self.fallback).pack(anchor="w")
        self.result_text = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.result_text.pack(anchor="w", pady=(10, 0))
        if ready and not building:
            self.result_text.config(text="✓ " + t["fallback_done"], foreground="#1a7f37")
        elif building and why != "tool":
            self.result_text.config(text=t["shaders_console_wait"], foreground="#555")

    def fallback_ready(self):
        import grapnel_core
        return grapnel_core.console_prepare_ready(os.path.join(self.out(), "data", "grapnel"))

    def fallback(self):
        """The console's own preparation (lot 82) set up beside the game: the census and the pipelines' list, the setting that
        offers it (grapnel_core.setup_console_prepare, which the build's step 7 runs by itself on a PC that does not prepare)"""
        import grapnel_core
        t, out = self.t, self.out()
        src, data = os.path.join(out, "grapnel-work", "shaders"), os.path.join(out, "data", "grapnel")
        census, states = grapnel_core.console_prep_names()
        try:
            r = grapnel_core.setup_console_prepare(data, os.path.join(src, census), os.path.join(src, states))
        except grapnel_core.BuildError as e:
            self.add_log("fallback: " + e.message)
            self.result_text.config(text=t["fallback_missing"], foreground="#b42318")
            return
        self.result_text.config(text="✓ " + t["fallback_done"], foreground="#1a7f37")
        self.add_log("fallback: %s and %s copied to %s, HOOKSHOT_PS5_PREPARE=1 in hookshot_env.txt%s"
                     % (r["files"][0], r["files"][1], data, "" if r["setting_added"] else " (there already)"))
        self.show(self.page)   # (the page as it now is: no button)

    def page_console(self):
        """Step 6: the title sent to the console over FTP (grapnel_send.py: the saves to this PC first, a confirmation before
        anything there is replaced, a send that goes on where it stopped, the sizes and the eboot's CRC32 read back)"""
        t = self.t
        self.heading(t["p_console"], t["console_intro"])
        row = ttk.Frame(self.body)
        row.pack(fill="x")
        ttk.Label(row, text=t["console_ip"]).pack(side="left")
        ttk.Entry(row, textvariable=self.ip_var, width=18).pack(side="left", padx=8)
        ttk.Button(row, text=t["console_test"], command=self.console_test).pack(side="left")
        self.console_note = ttk.Label(self.body, text="", wraplength=760, justify="left")
        self.console_note.pack(anchor="w", pady=(6, 0))
        ttk.Label(self.body, text=t["console_place"]).pack(anchor="w", pady=(8, 0))
        self.place_box = ttk.Frame(self.body)
        self.place_box.pack(anchor="w", padx=12)
        self.place_radios = {}
        for q in (grapnel_send.INTERNAL, "/mnt/ext1"):
            self.place_radio(q)
        flat = self.flat_copy_path()
        nup_text = t["console_nup"] if not flat else t["console_nup_flat"].format(gb=self.gb(os.path.getsize(flat)))
        ttk.Checkbutton(self.body, text=nup_text, variable=self.send_nup_var).pack(anchor="w", pady=(8, 0))
        self.send_btn = ttk.Button(self.body, text=t["console_send"], command=self.console_send, state="disabled")
        self.send_btn.pack(anchor="w", pady=8)
        self.progress_page()
        ttk.Label(self.body, text=t["console_manual"], wraplength=760, justify="left", foreground="#555").pack(anchor="w", pady=(14, 4))
        if flat:
            ttk.Label(self.body, text=t["console_manual_flat"].format(path=flat), wraplength=760, justify="left",
                      foreground="#9a6700").pack(anchor="w", pady=(0, 4))
        row = ttk.Frame(self.body)
        row.pack(anchor="w")
        ttk.Button(row, text=t["open_out"], command=self.open_out).pack(side="left")
        ttk.Button(row, text=t["open_install"], command=self.open_install).pack(side="left", padx=6)
        if self.console_found and self.console_found[0] == self.ip_var.get().strip():
            self.on_console_probe(*self.console_found)
        elif self.ip_var.get().strip() and not self.running:   # (the address of last time: tested on arrival)
            self.console_test()
        if self.running and self.running[0] == "console":
            self.send_btn.config(state="disabled")

    def flat_copy_path(self):
        """The flat copy the console gets, when the .nup's game files are compressed and the copy is made (else None)"""
        if not self.needs_flat():
            return None
        import grapnel_flat
        st = grapnel_flat.state(self.nup_var.get().strip(), self.flat_dir())
        return st["dst"] if st["kind"] == "done" else None

    def place_name(self, place):
        return self.t["place_internal"] if place == grapnel_send.INTERNAL else self.t["place_ext"].format(vol=place)

    def place_radio(self, place):
        """One choice of where to install (made when the page is, /mnt/ext0 too once the console has it)"""
        if place in self.place_radios or not hasattr(self, "place_box") or not self.place_box.winfo_exists():
            return
        r = ttk.Radiobutton(self.place_box, text=self.place_name(place), value=place, variable=self.place_var,
                            command=lambda: setattr(self, "place_touched", True))
        r.pack(anchor="w")
        self.place_radios[place] = r

    def ftpsrv_elf(self):
        p = os.path.join(self.parts.get("toolchain") or "", "ftpsrv", "ftpsrv-ps5.elf")
        return p if os.path.isfile(p) else None

    def console_test(self):
        ip = self.ip_var.get().strip()
        if not ip:
            return
        self.console_note.config(text=self.t["console_testing"], foreground="#555")
        threading.Thread(target=lambda: self.events.put(("console_probe", ip, probe_places(ip))), daemon=True).start()

    def on_console_probe(self, ip, found):
        self.console_found = (ip, found)
        if not hasattr(self, "console_note") or not self.console_note.winfo_exists():
            return
        t = self.t
        if found["ftp"]:
            text, ok = t["console_ftp"], True
        elif found["elfldr"]:
            ok = self.ftpsrv_elf() is not None
            text = t["console_elfldr"] if ok else t["console_no_ftpsrv"]
        else:
            text, ok = t["console_none"], False
        if found.get("volumes") is not None:   # (the FTP server answered: the disks and where the title is are known)
            vols = found["volumes"]
            for q in vols:
                self.place_radio(q)
            for q, r in self.place_radios.items():
                r.config(state="normal" if q == grapnel_send.INTERNAL or q in vols else "disabled")
            text += "\n" + (t["console_vols"].format(vols=", ".join(vols)) if vols else t["console_vol_none"])
            at = found["where"]["title"]
            if at:
                text += "\n" + t["console_title_at"].format(where=", ".join(self.place_name(q) for q in at))
            if not self.place_touched:   # (by default: where the title is, else the internal storage)
                self.place_var.set(at[0] if at else grapnel_send.INTERNAL)
            if self.place_var.get() not in [grapnel_send.INTERNAL] + vols:
                self.place_var.set(grapnel_send.INTERNAL)
        elif found["elfldr"] and not found["ftp"]:
            text += "\n" + t["console_vol_later"]
        if ok and not built(self.out()).get("game"):
            text, ok = text + "\n" + t["console_not_built"], False
        self.console_note.config(text=text, foreground="#1a7f37" if ok else "#b42318")
        self.send_btn.config(state="normal" if ok and not self.running else "disabled")
        if ok and getattr(self.args, "auto_send", False) and not self.running and not getattr(self, "auto_sent", False):
            self.auto_sent = True
            self.console_send()

    def console_send(self):
        if self.running:
            return
        ip, out, place = self.ip_var.get().strip(), self.out(), self.place_var.get() or grapnel_send.INTERNAL
        nup = self.nup_var.get().strip() if self.send_nup_var.get() else None
        self.save_settings()
        self.console_stop, self.console_answer = threading.Event(), queue.Queue()
        self.running = ("console", time.time())
        self.cancel_btn.config(state="normal")
        self.back_btn.config(state="disabled")
        self.send_btn.config(state="disabled")
        self.result_text.config(text="")
        self.update_next()
        keep_awake(True)
        self.add_log("console: sending to %s, %s" % (ip, place))
        threading.Thread(target=self.console_worker, args=(ip, out, nup, self.ftpsrv_elf(), place, self.flat_dir()),
                         daemon=True).start()

    def console_worker(self, ip, out, nup, elf, place=grapnel_send.INTERNAL, flat_dir=None):
        t, put = self.t, self.events.put
        try:
            if nup:   # (a .nup whose game files are compressed: its flat copy, which the console reads)
                nup, note = grapnel_send.console_game_file(nup, out, flat_dir)
                if note:
                    put(("line", "console: " + note))
            found = grapnel_send.probe(ip)
            if not found["ftp"]:
                if not (found["elfldr"] and elf):
                    raise grapnel_send.SendError(t["console_none"])
                put(("console_info", t["console_ftpsrv_sent"]))
                grapnel_send.send_elf(ip, elf)
                if not grapnel_send.wait_ftp(ip):
                    raise grapnel_send.SendError(t["console_ftp_late"])
            c = grapnel_send.Console(ip, root=os.environ.get("GRAPNEL_TEST_CONSOLE_ROOT", ""))   # (tests: a folder for its /)
            try:
                if place != grapnel_send.INTERNAL and place not in grapnel_send.volumes(c):
                    raise grapnel_send.SendError(t["console_no_volume"].format(vol=place))
                p = grapnel_send.plan(c, out, nup, place)
                if p["title_elsewhere"]:
                    put(("console_info", t["console_elsewhere"].format(old=", ".join(p["title_elsewhere"]))))
                saved = None
                if p["saves"]:
                    put(("console_info", t["console_saving"]))
                    saved, count = grapnel_send.backup_saves(c, out, p["saves"])
                    put(("console_info", t["console_saved"].format(n=count, dest=saved)))
                if p["replace"]:
                    put(("console_ask", p["replace"], saved))
                    if not self.console_answer.get():
                        raise grapnel_send.Cancelled("not replaced")
                grapnel_send.send(c, out, nup if p["nup"] and p["nup"]["send"] else None,
                                  report=lambda *a: put(("console_progress",) + a), cancel=self.console_stop,
                                  log=lambda line: put(("line", "console: " + line)), host=ip, place=place, nup_at=p["nup_at"])
                put(("console_info", t["console_verifying"]))
                bad, crc = grapnel_send.verify(c, out, nup, place, p["nup_at"])
                notes = []
                if not bad:   # (what is left at the other place: removed only when asked, once the send is read back)
                    new = self.place_name(place)
                    for old in p["title_elsewhere"]:
                        put(("console_question", t["console_move_title_title"], t["console_move_title"].format(new=new, old=old)))
                        if self.console_answer.get():
                            grapnel_send.remove_title(c, old, out, ip)
                            notes.append(t["console_removed"].format(what=old))
                        else:
                            notes.append(t["console_kept"].format(what=old))
                    for old, size in p["nup_elsewhere"]:
                        put(("console_question", t["console_move_nup_title"],
                             t["console_move_nup"].format(new=new, old=old, gb=size / 1e9)))
                        if self.console_answer.get():
                            grapnel_send.remove_nup(c, old)
                            notes.append(t["console_removed"].format(what=old))
                        else:
                            notes.append(t["console_kept"].format(what=old))
                put(("console_done", bad, crc, None, place, notes))
            finally:
                c.close()
        except grapnel_send.Cancelled:
            put(("console_done", None, None, "cancelled"))
        except (grapnel_send.SendError, OSError, EOFError, grapnel_send.ftplib.Error) as e:
            put(("console_done", None, None, str(e) or e.__class__.__name__))

    def on_console_info(self, text):
        self.add_log("console: " + text)
        if hasattr(self, "step_text") and self.step_text.winfo_exists():
            self.step_text.config(text=text)

    def on_console_ask(self, replace, saved):
        t = self.t
        text = t["console_ask"].format(what="\n".join("• " + r for r in replace),
                                       saved=t["console_ask_saved"].format(dest=saved) if saved else "")
        self.add_log("console: " + text.replace("\n", " "))
        self.console_answer.put(True if getattr(self.args, "auto_send", False) else messagebox.askyesno(t["console_ask_title"], text))

    def on_console_question(self, title, text):
        self.add_log("console: " + text.replace("\n", " "))
        self.console_answer.put(True if getattr(self.args, "auto_send", False) else messagebox.askyesno(title, text))

    def on_console_progress(self, done, total, rate, left, current):
        if not hasattr(self, "bar") or not self.bar.winfo_exists():
            return
        self.bar.config(mode="determinate", value=int(1000 * done / total) if total else 1000)
        self.step_text.config(text=current)
        self.left_text.config(text=self.t["console_rate"].format(mb=rate / 1e6, m=minutes(left) if left else "?"))

    def on_console_done(self, bad, crc, err, place=grapnel_send.INTERNAL, notes=()):
        t = self.t
        self.running = None
        keep_awake(False)
        self.cancel_btn.config(state="disabled")
        self.back_btn.config(state="normal")
        if err == "cancelled":
            text, colour = t["console_cancelled"], "#555"
        elif err:
            text, colour = t["console_failed"].format(e=err), "#b42318"
        elif bad:
            text, colour = t["console_bad"].format(what="\n".join(bad[:8])), "#b42318"
        else:
            homebrew = grapnel_send.place_paths(place)[0].rsplit("/", 1)[0]
            text, colour = t["console_ok"].format(crc="%d, 0x%08X" % (crc, crc), homebrew=homebrew), "#1a7f37"
            if notes:
                text += "\n\n" + "\n".join(notes)
            if hasattr(self, "bar") and self.bar.winfo_exists():
                self.bar.config(value=1000)
        self.add_log("console: " + text.replace("\n", " "))
        if hasattr(self, "result_text") and self.result_text.winfo_exists():
            self.result_text.config(text=text, foreground=colour)
            self.left_text.config(text="")
            self.step_text.config(text="")
        if hasattr(self, "send_btn") and self.send_btn.winfo_exists():
            self.send_btn.config(state="normal")
        self.update_next()

    # ---- running the builder ----

    def command(self, kind):
        p = self.parts
        cmd = [p["python"], "-I", "-u", os.path.join(HERE, "grapnel_build.py"), "--events", "json", "--python", p["python"]]
        if p.get("prompt_python"):
            cmd += ["--prompt-python", p["prompt_python"]]
        for opt in ("llvm", "toolchain"):
            if p.get(opt):
                cmd += ["--" + opt, p[opt]]
        if self.args.lock:
            cmd += ["--lock", self.args.lock]
        if kind == "flatten":
            return cmd + ["--flatten", "--nup", self.nup_var.get().strip(), "--flat-dir", self.flat_dir()]
        if kind == "check":
            cmd += ["--check", "--nup", self.nup_var.get().strip(), "--flat-dir", self.flat_dir()]
            return cmd + ([] if self.can_prepare else ["--no-prepare"])
        cmd += ["--nup", self.nup_var.get().strip(), "--out", self.out(), "--flat-dir", self.flat_dir()]
        if not self.icons_var.get():
            cmd.append("--no-prompts")
        try:
            mem = float(self.memory_var.get() or 0)
        except ValueError:
            mem = 0
        if mem > 0:
            cmd += ["--memory", "%g" % mem]
        if kind == "build-only" or not self.can_prepare:
            cmd.append("--no-prepare")
        return cmd

    def run_build(self, chain):
        if self.running:
            return
        total, _ = memory_gb()
        if total and total < MEMORY_GB and not messagebox.askyesno(self.t["err_title"], self.t["warn_memory"].format(total=total)):
            return
        self.save_settings()
        self.start("build", self.command("build" if chain else "build-only"))

    def run_shaders(self):
        if not self.running:
            if getattr(self, "shaders_btn", None) is not None and self.shaders_btn.winfo_exists():
                self.shaders_btn.destroy()   # (while it runs, and after: the page says how it went)
            self.start("shaders", self.command("build"))

    def start(self, kind, cmd):
        self.last_error, self.result, self.cancelled = None, None, False
        self.core_step, self.eta, self.step_frac = 0, None, None
        self.game_step_started = time.time()
        self.job = Job()
        self.add_log("$ " + " ".join('"%s"' % c if " " in c else c for c in cmd))
        try:
            proc = self.job.start(cmd, cwd=ROOT)
        except OSError as e:
            messagebox.showerror(self.t["err_title"], str(e))
            return
        self.running = (kind, time.time())
        self.cancel_btn.config(state="normal")
        self.back_btn.config(state="disabled")
        self.update_next()
        if kind != "check":
            keep_awake(True)

        def read():
            for raw in proc.stdout:
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                if line.startswith("@@json "):
                    try:
                        self.events.put(("ev", json.loads(line[7:])))
                        continue
                    except ValueError:
                        pass
                self.events.put(("line", line))
            self.events.put(("exit", proc.wait()))
        threading.Thread(target=read, daemon=True).start()

    def cancel(self):
        if self.running and messagebox.askyesno(self.t["err_title"], self.t["confirm_cancel"]):
            if self.running[0] == "console":   # (the send stops at its next block; sending again goes on from there)
                self.console_stop.set()
                return
            self.cancelled = True
            self.job.stop()

    def on_close(self):
        if self.running:
            if not messagebox.askyesno(self.t["err_title"], self.t["confirm_quit"]):
                return
            if self.running[0] == "console":
                self.console_stop.set()
            else:
                self.job.stop()
        keep_awake(False)
        self.save_settings()
        self.root.destroy()

    # ---- what the builder says ----

    def pump(self):
        try:
            for _ in range(2000):
                item = self.events.get_nowait()
                getattr(self, "on_" + item[0])(*item[1:])
        except queue.Empty:
            pass
        self.flush_log()
        if self.running and self.running[0] != "check" and hasattr(self, "time_text") and self.time_text.winfo_exists():
            secs = int(time.time() - self.running[1])
            self.time_text.config(text=self.t["elapsed"].format(t="%d:%02d:%02d" % (secs // 3600, secs // 60 % 60, secs % 60)))
        self.root.after(200, self.pump)

    def add_log(self, line):
        self.session_log.append(line)
        self._pending.append(line)

    def flush_log(self):
        if not self._pending:
            return
        pending, self._pending = self._pending, []
        self.log.configure(state="normal")
        self.log.insert("end", "\n".join(pending) + "\n")
        lines = int(self.log.index("end-1c").split(".")[0])
        if lines > 4000:
            self.log.delete("1.0", "%d.0" % (lines - 3000))
        self.log.see("end")
        self.log.configure(state="disabled")

    def on_line(self, line):
        self.add_log(line)

    def on_ev(self, ev):
        kind = ev.get("e")
        if kind == "log":
            self.add_log("   " + ev.get("line", ""))
        elif kind == "info":
            text = ev.get("text", "")
            self.add_log(text)
            if self.running and self.running[0] == "check":
                for prefix, row in self.INFO:
                    if text.startswith(prefix):
                        self.set_row(row, True, self.summary(row, text))
                        break
        elif kind == "step":
            self.core_step = ev["index"]
            self.step_frac = None
            name = self.t["core_steps"].get(ev["name"], ev["name"])
            self.add_log("== %d/%d %s" % (ev["index"], ev["total"], name))
            if self.core_step == PREPARE_STEP and PAGES[self.page] == "build":
                self.done.add(self.page)
                self.show(4)
            self.game_step_started = time.time()
            # (the shaders' page keeps its step's name: the notes written after the preparation are not the shaders)
            if hasattr(self, "step_text") and self.step_text.winfo_exists() and not (
                    PAGES[self.page] == "shaders" and self.core_step > PREPARE_STEP):
                self.step_text.config(text=name)
            if not (PAGES[self.page] == "shaders" and self.core_step > PREPARE_STEP):
                self.paint_progress(None, "")
        elif kind == "progress":
            # (the shaders' page while the game itself is compiled, its objects of another compiler or its sources changed: the
            # game's page again, as long as that lasts; the preparation's step brings the shaders' page back)
            if PAGES[self.page] == "shaders" and self.core_step and self.core_step < PREPARE_STEP and \
                    time.time() - getattr(self, "game_step_started", time.time()) > 15:
                build = PAGES.index("build")
                self.done.discard(build)
                self.done.discard(PAGES.index("shaders"))
                self.show(build)
            self.paint_progress(ev.get("fraction"), ev.get("text", ""))
        elif kind == "error":
            self.last_error = ev
        elif kind == "done":
            self.result = ev

    def paint_progress(self, fraction, text):
        if not hasattr(self, "bar") or not self.bar.winfo_exists() or not self.running:
            return
        t = self.t
        if self.running[0] == "flatten":   # (the flat copy: its own measure, written then read back)
            if fraction is not None:
                self.bar.config(mode="determinate", value=int(fraction * 1000))
                elapsed = time.time() - self.running[1]
                if fraction >= 0.02 and elapsed >= 5:
                    left = elapsed * (1 - fraction) / fraction
                    self.left_text.config(text=t["left_soon"] if left < 60 else t["left"].format(m=minutes(left)))
            if text and hasattr(self, "step_text") and self.step_text.winfo_exists():
                for k, v in t["flat_progress"].items():   # (the core's words, in the window's language)
                    text = text.replace(k, v)
                if self.lang == "fr":
                    text = re.sub(r"(\d)\.(\d)", r"\1,\2", text)
                self.step_text.config(text=text)
            return
        if self.core_step == PREPARE_STEP:   # (the shaders: the renderer's own measure of what is left)
            if fraction is not None:
                self.bar.config(mode="determinate", value=int(fraction * 1000))
            m = LEFT.search(text or "")
            if m:
                self.left_text.config(text=m.group(1).strip())
            return
        if fraction is not None:
            self.step_frac = fraction
        done = sum(GAME_WEIGHT.get(i, 0) for i in range(1, self.core_step))
        share = GAME_WEIGHT.get(self.core_step, 0) * (self.step_frac or 0)
        whole = done + share
        total = sum(GAME_WEIGHT.values())
        f = min(0.999, whole / total)
        self.bar.config(mode="determinate", value=int(f * 1000))
        elapsed = time.time() - self.running[1]
        if f >= 0.04 and elapsed >= 90:
            left = elapsed * (1 - f) / f
            self.eta = left if self.eta is None else 0.7 * self.eta + 0.3 * left
        shown = self.eta if self.eta is not None else max(60, GAME_MINUTES * 60 - elapsed)
        self.left_text.config(text=t["left_soon"] if shown < 60 else t["left"].format(m=minutes(shown)))

    def on_exit(self, rc):
        kind, started = self.running
        self.running = None
        keep_awake(False)
        self.cancel_btn.config(state="disabled")
        self.back_btn.config(state="normal" if self.page > 0 else "disabled")
        self.flush_log()
        self.write_session_log()
        t = self.t
        if self.cancelled:
            self.cancelled = False
            self.say_result(t["cancelled"], "#555")
            if kind == "flatten":   # (a stopped copy: said as started, it carries on)
                self.read_flat()
            return self.update_next()
        if kind == "flatten" and rc == 0 and self.result:
            r = self.result.get("summary") or {}
            if not r.get("reused"):
                self.flat_made = dict(r, path=self.result.get("path", ""))
            if hasattr(self, "bar") and self.bar.winfo_exists():
                self.bar.config(mode="determinate", value=1000)
                self.left_text.config(text="")
            self.read_flat()
            return self.update_next()
        if kind == "flatten":
            self.read_flat()
        if kind == "check":
            err = self.last_error or {}
            ok = rc == 0 and self.result is not None and not self.check_problems
            if not ok and err.get("code"):
                row = self.CODE_ROW.get(err["code"], "compile" if err["code"] in ("tool-failed",) else "toolchain")
                self.set_row(row, False, t["errors"].get(err["code"]) or err.get("message", "")[:300])
            elif not ok and not self.check_problems:
                self.set_row("compile", False, t["worker"].format(rc=rc))
            if hasattr(self, "check_note") and self.check_note.winfo_exists():
                self.check_note.config(text=t["check_ok"] if ok else t["check_bad"], foreground="#1a7f37" if ok else "#b42318")
            self.check_state = (self.inputs(), ok)
            return self.update_next()
        if rc == 0 and self.result:
            b = built(self.out())
            if (b.get("console") or {}).get("why") == "illegal-instruction":   # (the PC's preparation stopped on an instruction
                self.can_prepare = False                                          # its processor does not have: the console's now)
            if PAGES[self.page] == "shaders" and not self.can_prepare:
                self.show(self.page)   # (what the build set up for the console, or the fallback button)
                return self.update_next()
            if PAGES[self.page] == "build":
                self.show_built(b)
            elif PAGES[self.page] == "shaders" and b.get("shaders") and hasattr(self, "result_text"):
                self.bar.config(mode="determinate", value=1000)
                if hasattr(self, "left_text") and self.left_text.winfo_exists():
                    self.left_text.config(text="")
                self.result_text.config(text=t["shaders_done"].format(entries=b["shaders"].get("entries")), foreground="#1a7f37")
            self.update_next()
            return
        err = self.last_error or {}
        code = err.get("code")
        text = t["errors"].get(code) or (err.get("message") if code else t["worker"].format(rc=rc))
        self.say_result("✗ " + text, "#b42318")
        details = "\n".join(x for x in (err.get("message", ""), err.get("details", "")) if x) or "\n".join(self.session_log[-12:])
        messagebox.showerror(t["err_title"], text + "\n\n" + t["err_details"].format(log=self.session_log_path() or "?") + "\n" +
                             details[-1500:])
        self.update_next()

    def say_result(self, text, colour):
        if hasattr(self, "result_text") and self.result_text.winfo_exists():
            self.result_text.config(text=text, foreground=colour)
        elif hasattr(self, "check_note") and self.check_note.winfo_exists():
            self.check_note.config(text=text, foreground=colour)

    # ---- the result, the report, the tools ----

    def session_log_path(self):
        out = self.out()
        return os.path.join(out, "grapnel-work", "logs", "builder-window.log") if out else None

    def write_session_log(self):
        path = self.session_log_path()
        if not path or not os.path.isdir(os.path.dirname(os.path.dirname(path))):
            return
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "a", encoding="utf-8") as f:
                f.write("\n".join(self.session_log) + "\n")
        except OSError:
            pass

    def report(self):
        import platform
        total, _ = memory_gb()
        lines = ["Grapnel builder report", "system: %s %s (%s), %s processors, memory %.1f GB" % (
            platform.system(), platform.release(), platform.version(), os.cpu_count(), total or 0)]
        lines.append("processor: %s, x86-64-v3 %s (%s)" % (self.cpu.get("name") or "?", "yes" if self.cpu["v3"] else "no",
                                                           self.cpu["read"]))
        version = os.path.join(ROOT, "VERSION")
        if os.path.isfile(version):
            lines.append("builder: " + open(version, encoding="utf-8").read().strip())
        try:
            with open(os.path.join(self.out(), "manifest.json"), encoding="utf-8") as f:
                m = json.load(f)
            lines.append("checkout: %s" % m.get("checkout"))
            for k, v in m["files"].items():
                if k.replace("\\", "/").endswith(("/eboot.bin", "/image.bin")):
                    lines.append("%s: %d bytes, CRC32 %d (0x%08X)%s" % (k.replace("\\", "/").rsplit("/", 1)[1], v["bytes"], v["crc32"],
                                                                      v["crc32"], (", sha256 " + v["sha256"]) if "sha256" in v else ""))
            for s in m.get("steps", []):
                lines.append("  %-45s %6d s" % (s["step"], s["seconds"]))
            lines.append("llvm: %s; toolchain: %s; prepared: %s" % (m.get("llvm"), m.get("toolchain"), m.get("prepared")))
            if m.get("console_prepare"):
                lines.append("the console prepares the shaders: %s" % m["console_prepare"].get("why"))
        except (OSError, ValueError, KeyError):
            pass
        found = (self.nup_state[3].get("game_files") if self.nup_state else None) or {}
        if found.get("reference"):   # (the dump against a sound dump's reference: corrupted and missing files)
            lines.append("game file: %d files checked against %s: %d corrupted, %d missing, %d not in the reference" % (
                found["checked"], found["reference"], len(found["corrupted"]), len(found["missing"]), found["unknown"]))
            lines += ["  corrupted: " + f for f in found["corrupted"][:40]] + ["  missing: " + f for f in found["missing"][:40]]
        if self.last_error:
            lines.append("error: %s: %s" % (self.last_error.get("code"), self.last_error.get("message")))
            lines += (self.last_error.get("details") or "").splitlines()[-12:]
        lines.append("log: %s" % self.session_log_path())
        return "\n".join(lines)

    def copy_report(self):
        self.root.clipboard_clear()
        self.root.clipboard_append(self.report())
        messagebox.showinfo(self.t["err_title"], self.t["report_copied"])

    def open_path(self, path):
        if WINDOWS:
            os.startfile(path)
        else:
            subprocess.Popen(["xdg-open", path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def open_out(self):
        if self.out() and os.path.isdir(self.out()):
            self.open_path(self.out())

    def open_install(self):
        p = os.path.join(self.out(), "INSTALL.%s.txt" % self.lang)
        if os.path.isfile(p):
            self.open_path(p)
        else:
            messagebox.showinfo(self.t["err_title"], self.t["no_install"])

    def crc_tool(self):
        p = filedialog.askopenfilename(title=self.t["crc_pick"], initialdir=self.out() or os.path.expanduser("~"))
        if p:
            threading.Thread(target=lambda: self.events.put(("crc", p, crc32_file(p))), daemon=True).start()

    def on_crc(self, path, c):
        self.root.clipboard_clear()
        self.root.clipboard_append("%s: CRC32 %d (0x%08X), %d bytes" % (os.path.basename(path), c, c, os.path.getsize(path)))
        messagebox.showinfo(self.t["err_title"], self.t["crc_result"].format(name=path, bytes=os.path.getsize(path), crc=c))


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Grapnel builder's assistant")
    ap.add_argument("--toolchain", help="the grapnel-toolchain folder (Windows: the builder's own)")
    ap.add_argument("--llvm", help="LLVM 19.1.7's folder (Windows: the builder's own)")
    ap.add_argument("--python", help="the Python for the build (default: this one)")
    ap.add_argument("--prompt-python", help="the Python for the button icons")
    ap.add_argument("--lock", help="(the port's PC) a lock file for the heavy steps")
    ap.add_argument("--screenshot-after", type=float, default=0, help="(tests) close the window after this many seconds")
    ap.add_argument("--page", type=int, default=0, help="(tests) open on this page (1 to 6), the ones before taken as done")
    ap.add_argument("--auto-send", action="store_true", help="(tests) step 6: send as soon as the console answers, and say yes "
                                                               "to the replacement and to the removals")
    ap.add_argument("--place", choices=grapnel_send.PLACES, help="(tests) step 6: where to install, as if chosen on the page")
    args = ap.parse_args()
    if WINDOWS:
        import ctypes
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (OSError, AttributeError):
            pass
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("Grapnel.Builder")
        except (OSError, AttributeError):
            pass
    if sys.stderr is None:   # (pythonw.exe: no console; what would go there goes to a file the player can send)
        try:
            os.makedirs(os.path.dirname(settings_path()), exist_ok=True)
            sys.stderr = sys.stdout = open(os.path.join(os.path.dirname(settings_path()), "window-errors.log"), "a", encoding="utf-8")
        except OSError:
            pass
    root = tk.Tk()

    def callback_error(*exc):
        import traceback
        text = "".join(traceback.format_exception(*exc))
        print(text, file=sys.stderr, flush=True)
        messagebox.showerror("Grapnel", text[-1500:])
    root.report_callback_exception = callback_error
    try:
        ttk.Style(root).theme_use("vista" if WINDOWS else "clam")
    except tk.TclError:
        pass
    app = Assistant(root, args)
    if args.page > 1:
        root.after(800, lambda: app.jump(args.page - 1))
    if args.screenshot_after:
        root.after(int(args.screenshot_after * 1000), root.destroy)
    root.mainloop()


if __name__ == "__main__":
    main()
