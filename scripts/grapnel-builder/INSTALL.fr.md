# Constructeur Grapnel 0.2 (Linux) : fabriquer Grapnel pour votre PS5

*English version: [INSTALL.md](INSTALL.md).*

*Sous Windows, prenez plutôt `Grapnel-Builder-0.2.0-windows-x64.zip` sur la page Releases : un dossier qui contient tout et
une fenêtre pas à pas (son LISEZMOI, et « Construire sous Windows » dans le README du dépôt). Ce guide-ci est celui de Linux.*

Grapnel est un portage sur PS5 de Longshot, la recompilation statique de *The Legend of Zelda: Ocarina of Time* (Nintendo
Switch 2 Edition). Le titre PS5 contient le code du jeu traduit et compilé : il ne peut pas être téléchargé. Vous le fabriquez
sur votre propre PC Linux, à partir de **votre propre copie du jeu**, avec ce constructeur. Rien de ce qu'il produit ne doit
être partagé.

## Ce qu'il faut

- **Votre fichier du jeu** : le dump `.nup` de *Zelda: Ocarina of Time*, Nintendo Switch 2 Edition, **avec sa mise à jour
  v131072** (titre `0400486024DEC800` ; le nom du fichier peut montrer à la place celui du jeu de base, `0400486024DEC000` :
  c'est normal). Le constructeur ne prend que ce `.nup`, et refuse tout autre jeu ou version.
- **Un PC Linux** (x86 64 bits, Ubuntu 24.04, Debian 13 ou plus récent), **8 Go de mémoire** au moins (16 Go ou plus va plus
  vite), **10 Go libres sur le disque** en plus du `.nup` (et environ 2 Go de plus pour LLVM), et de 45 minutes à 2 heures et
  demie.
- **LLVM 19.1.7** exactement (tous les joueurs compilent avec le même compilateur : chaque eboot est celui que le projet a
  essayé), **Python 3.12 ou plus récent** (les paquets du constructeur l'exigent) et **git** : comment les avoir, ci-dessous.
- **La chaîne d'outils console**, `grapnel-toolchain-0.3-linux-x64.tar.xz`, sur la page **Releases** du dépôt Grapnel
  (<https://github.com/morpheus302/PS5_Grapnel/releases>) : le SDK de charges PS5, les pièces de lien de PS5_Vulkan, le pilote
  RADV, les en-têtes du rendu, dav1d, et `prepare/`, la préparation des shaders pour le PC. Aucun fichier ni aucun code du jeu
  dedans.
- Sur la console : un débridage avec chargeur d'ELF et accès à `/data`, et FTP (ou un autre moyen de copier des fichiers).
  Grapnel se joue sur une PS5 standard (firmware jusqu'à 12.60). **La PS5 Pro n'a jamais été essayée.**

## Installer les outils (une fois)

1. **LLVM 19.1.7, git et le module `venv` de Python.**
   - Ubuntu 24.04 : les paquets de LLVM lui-même, sur <https://apt.llvm.org> (le `clang-19` d'Ubuntu est la 19.1.1, que le
     constructeur refuse ; le LLVM 19 d'apt.llvm.org est la 19.1.7, avec tout ce qu'il faut au constructeur) :

     ```sh
     sudo apt update
     sudo apt install git python3-venv wget lsb-release software-properties-common gnupg
     wget https://apt.llvm.org/llvm.sh
     chmod +x llvm.sh
     sudo ./llvm.sh 19
     ```
   - Debian 13 (son LLVM 19 est la 19.1.7) :

     ```sh
     sudo apt install git python3-venv clang-19 lld-19 llvm-19 libclang-rt-19-dev
     ```
   - Les autres : l'archive de LLVM 19.1.7 pour Linux x86-64 sur
     <https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.7>, décompressée où vous voulez (puis donnez au
     constructeur `--llvm <ce dossier>`), avec git et Python 3.12 ou plus récent (et son module `venv`) de votre système.
2. **Le dépôt Grapnel**, avec `git clone` (pour que la construction porte sa version exacte ; un téléchargement ZIP dit
   `unknown`) :

   ```sh
   git clone https://github.com/morpheus302/PS5_Grapnel
   cd PS5_Grapnel
   ```
   Toutes les commandes qui suivent se lancent dans ce dossier.
3. **Les paquets Python du constructeur**, dans un environnement Python à eux (Ubuntu et Debian refusent `pip install` hors
   d'un environnement) :

   ```sh
   python3 -m venv ~/grapnel-venv
   ~/grapnel-venv/bin/pip install -r scripts/grapnel-builder/requirements.txt
   ```
4. **La chaîne d'outils console** : téléchargez `grapnel-toolchain-0.3-linux-x64.tar.xz` sur la page Releases, puis
   décompressez-la, ici dans votre dossier personnel (cela fait `~/grapnel-toolchain-0.3-linux-x64`) :

   ```sh
   tar -xf ~/Téléchargements/grapnel-toolchain-0.3-linux-x64.tar.xz -C ~
   ```
   Gardez le dossier tel qu'il sort de l'archive, et modifiable : chaque construction vérifie ses fichiers, et y écrit un
   fichier à elle.

## Vérifier, puis construire

Depuis le dossier `PS5_Grapnel` :

```sh
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --check --toolchain ~/grapnel-toolchain-0.3-linux-x64
~/grapnel-venv/bin/python -I scripts/grapnel-builder/grapnel_build.py --nup ~/jeux/zelda-oot.nup --out ~/grapnel-out \
    --toolchain ~/grapnel-toolchain-0.3-linux-x64
```

Un fichier du jeu dont le nom a des espaces ou des crochets se met entre guillemets, avec `$HOME` plutôt que `~` (le shell ne
remplace pas un `~` entre guillemets) : `--nup "$HOME/jeux/mon jeu [v131072].nup"`.

`--check` n'a pas besoin du jeu et dit ce qui manque : tout va bien quand il finit par `@@done check` (une ligne qui commence
par `note:` dit une chose bonne à savoir, pas une erreur). La construction affiche ses huit étapes et finit par
`@@done <out>` ; les plus longues sont la
compilation (environ 25 minutes sur un PC récent à 8 cœurs) et **la préparation des shaders** (environ 15 minutes sur un PC
récent à 8 cœurs, jusqu'à 45 sur un petit ; 2 Go de mémoire libre au moins) : le PC compile les shaders du jeu pour le pilote de
la console, sur tous ses cœurs, autant à la fois que sa mémoire libre le permet, et affiche le temps restant. Avec peu de
mémoire, ajoutez `--lift-jobs 1 --memory 4` : plus lent, jamais différent. Reconstruire après une mise à jour du dépôt est bien
plus court (les fichiers de travail restent dans `<out>/grapnel-work` ; la préparation n'est refaite que si le rendu ou les
listes ont changé).

## Copier sur la console

`<out>/INSTALL.fr.txt` le redit pour ce qui a été construit :

1. `<out>/homebrew/GRPN00001` → `/data/homebrew/GRPN00001` (avec son `radv-shader-cache/`, les shaders préparés : ~210 Mo),
   puis enregistrez le titre avec vos outils homebrew : il apparaît sous le nom **Grapnel** à l'accueil. **Après une nouvelle
   construction**, effacez le `/data/homebrew/GRPN00001/radv-shader-cache` de la console avant de copier le dossier.
2. `<out>/data/grapnel` → `/data/grapnel` (`image.bin`, `hookshot_env.txt`, `skip.txt`, `prompts/`,
   `hookshot_prepare_progress.txt`, et `hookshot_menu_fond.rgba`, l'image de l'écran de lancement).
   **Si la console a déjà `/data/hookshot`** (un portage Hookshot) : ne créez pas
   `/data/grapnel` ; copiez `image.bin`, `prompts/` et `hookshot_prepare_progress.txt` dans `/data/hookshot`. Le premier
   lancement de Grapnel le renomme en `/data/grapnel`, sauvegardes comprises ; un `/data/grapnel` créé avant serait pris à sa
   place, et les sauvegardes laissées.
3. Votre `.nup`, tel quel → `/data/grapnel/game.nup`.
4. Lancez Grapnel. **Les shaders sont prêts** : le constructeur les a préparés sur votre PC, dans le cache même du pilote de
   la console (le `radv-shader-cache` du dossier du titre), pour que le jeu ne s'arrête pas pour les compiler en jouant (chaque
   compilation en jeu est un arrêt d'une seconde et plus, et de la mémoire que la console ne récupère jamais). La console ne
   prépare rien : l'écran de lancement vérifie seulement le cache au démarrage (sa page SHADERS dit « Prêts »). Un pipeline
   qu'aucune liste ne connaît est encore compilé à sa première apparition.

`eboot.bin` et `image.bin` vont ensemble : après chaque nouvelle construction, recopiez les deux.

## Privé

`<out>/` et son `grapnel-work/` contiennent le code et les données du jeu faits depuis votre copie (`radv-shader-cache/`, ce
sont les shaders du jeu compilés, `grapnel-work/shaders/` leur code) : **ne les partagez, ne les
envoyez et ne les publiez jamais**, pas même en partie. Pour signaler un problème : le journal
(`/data/grapnel/hookshot_log.txt`) et `<out>/manifest.json` (tailles et sommes de contrôle seulement) peuvent être joints ;
rien d'autre de `<out>`.

## Ce que fait le constructeur

Il lit le programme du jeu dans le `.nup` (40 Mo de ses 25,6 Go), le traduit en C avec le traducteur du portage, le compile avec
le SDK de la console, le lie avec le runtime et le rendu de Grapnel, signe l'eboot et range le dossier du titre (la mémoire du
titre, 580 Mio de mémoire flexible, est fixe), puis fabrique les icônes des boutons PlayStation à partir de la police et des
textures du jeu lui-même. Enfin, il prépare les shaders : il fait la liste des shaders du jeu (depuis les ~290 Mo du `.nup` qui
les contiennent) et des pipelines à construire (depuis les listes publiques de ce dépôt : aucun fichier du jeu), puis lance **la
préparation des shaders de la console elle-même sur votre PC**, sur un modèle du pilote de la console construit pour Linux (le
même pilote, donc les mêmes clés de cache), et vérifie le cache qu'il a fait : chaque partie entière, et une seconde passe qui
refait tout et trouve tout, sans rien compiler. Le `prepare/` de la chaîne d'outils ne contient aucun code du jeu. Linux x86-64
seulement pour l'instant ; `--no-prepare` s'en passe (la console compile alors en jouant). Sa source est
`scripts/grapnel-builder/grapnel_build.py` ; sa vérification : deux constructions depuis des dossiers différents donnent le même
eboot, à l'octet près.
