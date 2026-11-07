using System.Globalization;

namespace HookshotBuilder;

/// One of the game's languages: its code in language.txt, its own name, and the app's text in it.
sealed record Language(string Code, string Name, string[] Text)
{
    public override string ToString() => Name;
}

enum S
{
    Subtitle, GameFiles, Nup, Folders, DropHint, SaveTo, Build, Browse, Needs, Free, Checking, VersionOk, VersionBad, ToolsMissing, PythonMissing,
    Building, Patience, Elapsed, Read, Lift, Compile, Finish, ShowDetails, HideDetails, Cancel, Back, OpenFolder, Play, Ready, BuiltIn,
    Stopped, Cancelled, CancelledNote, CloseQuestion, LanguageLabel,
    // (the update, English only for now: Strings.Text falls back to it)
    Download, Updating, UpdateFailed,
}

static class Strings
{
    static readonly string[] En =
    {
        "Turns your own copy of Ocarina of Time into a game folder you can start on PC.", "Game files", ".nup file", "romfs and exefs",
        "You can also drop a file or folder onto this window.", "Save the game to", "Build", "Browse", "Takes about 45 minutes and needs 60 GB.",
        "{0} has {1} GB free.", "Checking the game version...", "This is the version Hookshot supports.", "This version isn't supported. Hookshot needs build e0f3ccfa.",
        "The Hookshot tools were not found next to this app.", "Python was not found.",
        "Building", "This takes a while, and your PC stays usable.", "Elapsed", "Reading your game files", "Translating the game to PC code", "Compiling",
        "Putting the game folder together", "Show details", "Hide details", "Cancel", "Back", "Open folder", "Play", "Your game is ready", "built in",
        "The build stopped", "Build cancelled", "You can start again from the same folder.", "Stop the build and close?", "Language",
        "Downloading the new version", "Updating your game", "The update could not be downloaded",
    };
    static readonly string[] Fr =
    {
        "Transforme votre copie d'Ocarina of Time en un dossier de jeu à lancer sur PC.", "Fichiers du jeu", "Fichier .nup", "romfs et exefs",
        "Vous pouvez aussi déposer un fichier ou un dossier sur cette fenêtre.", "Enregistrer le jeu dans", "Créer", "Parcourir", "Prend environ 45 minutes et nécessite 60 Go.",
        "{0} : {1} Go libres.", "Vérification de la version du jeu...", "Cette version est prise en charge par Hookshot.", "Cette version n'est pas prise en charge. Hookshot a besoin de la version e0f3ccfa.",
        "Les outils Hookshot sont introuvables à côté de cette application.", "Python est introuvable.",
        "Création en cours", "Cela prend du temps, et votre PC reste utilisable.", "Temps écoulé", "Lecture de vos fichiers de jeu", "Traduction du jeu en code PC", "Compilation",
        "Assemblage du dossier du jeu", "Afficher les détails", "Masquer les détails", "Annuler", "Retour", "Ouvrir le dossier", "Jouer", "Votre jeu est prêt", "créé en",
        "La création s'est arrêtée", "Création annulée", "Vous pouvez recommencer depuis le même dossier.", "Arrêter la création et fermer ?", "Langue",
    };
    static readonly string[] De =
    {
        "Macht aus deiner eigenen Kopie von Ocarina of Time einen Spielordner für den PC.", "Spieldateien", ".nup-Datei", "romfs und exefs",
        "Du kannst auch eine Datei oder einen Ordner auf dieses Fenster ziehen.", "Spiel speichern unter", "Erstellen", "Durchsuchen", "Dauert etwa 45 Minuten und benötigt 60 GB.",
        "{0} hat {1} GB frei.", "Spielversion wird geprüft...", "Diese Version wird von Hookshot unterstützt.", "Diese Version wird nicht unterstützt. Hookshot benötigt Build e0f3ccfa.",
        "Die Hookshot-Werkzeuge wurden neben dieser App nicht gefunden.", "Python wurde nicht gefunden.",
        "Wird erstellt", "Das dauert eine Weile, dein PC bleibt nutzbar.", "Vergangen", "Spieldateien werden gelesen", "Spiel wird in PC-Code übersetzt", "Kompilieren",
        "Spielordner wird zusammengestellt", "Details anzeigen", "Details ausblenden", "Abbrechen", "Zurück", "Ordner öffnen", "Spielen", "Dein Spiel ist fertig", "erstellt in",
        "Die Erstellung wurde angehalten", "Erstellung abgebrochen", "Du kannst im selben Ordner neu beginnen.", "Erstellung beenden und schließen?", "Sprache",
    };
    static readonly string[] It =
    {
        "Trasforma la tua copia di Ocarina of Time in una cartella di gioco da avviare su PC.", "File di gioco", "File .nup", "romfs ed exefs",
        "Puoi anche trascinare un file o una cartella su questa finestra.", "Salva il gioco in", "Crea", "Sfoglia", "Richiede circa 45 minuti e 60 GB.",
        "{0} ha {1} GB liberi.", "Controllo della versione del gioco...", "Questa versione è supportata da Hookshot.", "Questa versione non è supportata. Hookshot richiede la build e0f3ccfa.",
        "Gli strumenti di Hookshot non sono stati trovati accanto a questa app.", "Python non è stato trovato.",
        "Creazione in corso", "Ci vuole un po', e il PC resta utilizzabile.", "Trascorso", "Lettura dei file di gioco", "Traduzione del gioco in codice PC", "Compilazione",
        "Assemblaggio della cartella di gioco", "Mostra dettagli", "Nascondi dettagli", "Annulla", "Indietro", "Apri cartella", "Gioca", "Il tuo gioco è pronto", "creato in",
        "La creazione si è interrotta", "Creazione annullata", "Puoi ricominciare dalla stessa cartella.", "Interrompere la creazione e chiudere?", "Lingua",
    };
    static readonly string[] Es =
    {
        "Convierte tu copia de Ocarina of Time en una carpeta de juego para iniciar en PC.", "Archivos del juego", "Archivo .nup", "romfs y exefs",
        "También puedes soltar un archivo o una carpeta en esta ventana.", "Guardar el juego en", "Crear", "Examinar", "Tarda unos 45 minutos y necesita 60 GB.",
        "{0} tiene {1} GB libres.", "Comprobando la versión del juego...", "Hookshot admite esta versión.", "Esta versión no es compatible. Hookshot necesita la build e0f3ccfa.",
        "No se encontraron las herramientas de Hookshot junto a esta aplicación.", "No se encontró Python.",
        "Creando", "Tarda un rato, y tu PC sigue siendo utilizable.", "Transcurrido", "Leyendo tus archivos del juego", "Traduciendo el juego a código de PC", "Compilando",
        "Preparando la carpeta del juego", "Mostrar detalles", "Ocultar detalles", "Cancelar", "Atrás", "Abrir carpeta", "Jugar", "Tu juego está listo", "creado en",
        "La creación se detuvo", "Creación cancelada", "Puedes empezar de nuevo desde la misma carpeta.", "¿Detener la creación y cerrar?", "Idioma",
    };
    static readonly string[] Pt =
    {
        "Transforma sua cópia de Ocarina of Time em uma pasta de jogo para iniciar no PC.", "Arquivos do jogo", "Arquivo .nup", "romfs e exefs",
        "Você também pode soltar um arquivo ou uma pasta nesta janela.", "Salvar o jogo em", "Criar", "Procurar", "Leva cerca de 45 minutos e precisa de 60 GB.",
        "{0} tem {1} GB livres.", "Verificando a versão do jogo...", "Esta versão é compatível com o Hookshot.", "Esta versão não é compatível. O Hookshot precisa da build e0f3ccfa.",
        "As ferramentas do Hookshot não foram encontradas ao lado deste aplicativo.", "O Python não foi encontrado.",
        "Criando", "Isso leva um tempo, e seu PC continua utilizável.", "Decorrido", "Lendo seus arquivos do jogo", "Traduzindo o jogo para código de PC", "Compilando",
        "Montando a pasta do jogo", "Mostrar detalhes", "Ocultar detalhes", "Cancelar", "Voltar", "Abrir pasta", "Jogar", "Seu jogo está pronto", "criado em",
        "A criação parou", "Criação cancelada", "Você pode recomeçar a partir da mesma pasta.", "Parar a criação e fechar?", "Idioma",
    };
    static readonly string[] Nl =
    {
        "Maakt van je eigen exemplaar van Ocarina of Time een spelmap om op pc te starten.", "Spelbestanden", ".nup-bestand", "romfs en exefs",
        "Je kunt ook een bestand of map op dit venster slepen.", "Spel opslaan in", "Maken", "Bladeren", "Duurt ongeveer 45 minuten en vereist 60 GB.",
        "{0} heeft {1} GB vrij.", "Spelversie controleren...", "Deze versie wordt door Hookshot ondersteund.", "Deze versie wordt niet ondersteund. Hookshot heeft build e0f3ccfa nodig.",
        "De Hookshot-tools zijn niet naast deze app gevonden.", "Python is niet gevonden.",
        "Bezig met maken", "Dit duurt even, en je pc blijft bruikbaar.", "Verstreken", "Je spelbestanden lezen", "Spel vertalen naar pc-code", "Compileren",
        "Spelmap samenstellen", "Details tonen", "Details verbergen", "Annuleren", "Terug", "Map openen", "Spelen", "Je spel is klaar", "gemaakt in",
        "Het maken is gestopt", "Maken geannuleerd", "Je kunt opnieuw beginnen vanuit dezelfde map.", "Maken stoppen en sluiten?", "Taal",
    };
    static readonly string[] Pl =
    {
        "Zamienia Twoją kopię Ocarina of Time w folder gry, który uruchomisz na PC.", "Pliki gry", "Plik .nup", "romfs i exefs",
        "Możesz też upuścić plik lub folder na to okno.", "Zapisz grę w", "Utwórz", "Przeglądaj", "Trwa około 45 minut i wymaga 60 GB.",
        "{0}: wolne {1} GB.", "Sprawdzanie wersji gry...", "Ta wersja jest obsługiwana przez Hookshot.", "Ta wersja nie jest obsługiwana. Hookshot wymaga kompilacji e0f3ccfa.",
        "Nie znaleziono narzędzi Hookshot obok tej aplikacji.", "Nie znaleziono Pythona.",
        "Tworzenie", "To chwilę potrwa, a komputer pozostanie w użyciu.", "Upłynęło", "Odczytywanie plików gry", "Tłumaczenie gry na kod PC", "Kompilowanie",
        "Składanie folderu gry", "Pokaż szczegóły", "Ukryj szczegóły", "Anuluj", "Wstecz", "Otwórz folder", "Graj", "Twoja gra jest gotowa", "utworzono w",
        "Tworzenie zostało przerwane", "Anulowano tworzenie", "Możesz zacząć ponownie z tego samego folderu.", "Przerwać tworzenie i zamknąć?", "Język",
    };
    static readonly string[] Ru =
    {
        "Превращает вашу копию Ocarina of Time в папку с игрой для запуска на ПК.", "Файлы игры", "Файл .nup", "romfs и exefs",
        "Также можно перетащить файл или папку в это окно.", "Сохранить игру в", "Собрать", "Обзор", "Занимает около 45 минут и требует 60 ГБ.",
        "{0}: свободно {1} ГБ.", "Проверка версии игры...", "Эта версия поддерживается Hookshot.", "Эта версия не поддерживается. Hookshot нужна сборка e0f3ccfa.",
        "Инструменты Hookshot не найдены рядом с этим приложением.", "Python не найден.",
        "Сборка", "Это займёт время, а компьютером можно пользоваться.", "Прошло", "Чтение файлов игры", "Перевод игры в код для ПК", "Компиляция",
        "Подготовка папки с игрой", "Показать подробности", "Скрыть подробности", "Отмена", "Назад", "Открыть папку", "Играть", "Ваша игра готова", "собрано за",
        "Сборка остановлена", "Сборка отменена", "Можно начать заново из той же папки.", "Остановить сборку и закрыть?", "Язык",
    };
    static readonly string[] Ja =
    {
        "お持ちの Ocarina of Time を、PC で起動できるゲームフォルダーに変換します。", "ゲームファイル", ".nup ファイル", "romfs と exefs",
        "ファイルやフォルダーをこのウィンドウにドロップすることもできます。", "ゲームの保存先", "ビルド", "参照", "約 45 分かかり、60 GB 必要です。",
        "{0} の空き容量: {1} GB", "ゲームのバージョンを確認しています...", "このバージョンは Hookshot に対応しています。", "このバージョンには対応していません。Hookshot にはビルド e0f3ccfa が必要です。",
        "このアプリの横に Hookshot ツールが見つかりません。", "Python が見つかりません。",
        "ビルド中", "時間がかかりますが、PC はそのまま使えます。", "経過時間", "ゲームファイルを読み込んでいます", "ゲームを PC 用コードに変換しています", "コンパイル中",
        "ゲームフォルダーを作成しています", "詳細を表示", "詳細を隠す", "キャンセル", "戻る", "フォルダーを開く", "プレイ", "ゲームの準備ができました", "所要時間",
        "ビルドが停止しました", "ビルドをキャンセルしました", "同じフォルダーからやり直せます。", "ビルドを中止して閉じますか?", "言語",
    };
    static readonly string[] Ko =
    {
        "가지고 있는 Ocarina of Time을 PC에서 실행할 수 있는 게임 폴더로 만듭니다.", "게임 파일", ".nup 파일", "romfs 및 exefs",
        "파일이나 폴더를 이 창에 끌어다 놓을 수도 있습니다.", "게임 저장 위치", "빌드", "찾아보기", "약 45분이 걸리며 60GB가 필요합니다.",
        "{0} 여유 공간: {1}GB", "게임 버전을 확인하는 중...", "Hookshot이 지원하는 버전입니다.", "지원되지 않는 버전입니다. Hookshot에는 빌드 e0f3ccfa가 필요합니다.",
        "이 앱 옆에서 Hookshot 도구를 찾을 수 없습니다.", "Python을 찾을 수 없습니다.",
        "빌드 중", "시간이 걸리지만 PC는 계속 사용할 수 있습니다.", "경과 시간", "게임 파일을 읽는 중", "게임을 PC 코드로 변환하는 중", "컴파일 중",
        "게임 폴더를 만드는 중", "자세히 보기", "자세히 숨기기", "취소", "뒤로", "폴더 열기", "플레이", "게임이 준비되었습니다", "소요 시간",
        "빌드가 중단되었습니다", "빌드가 취소되었습니다", "같은 폴더에서 다시 시작할 수 있습니다.", "빌드를 중단하고 닫을까요?", "언어",
    };
    static readonly string[] ZhHans =
    {
        "将你自己的 Ocarina of Time 变成可在电脑上启动的游戏文件夹。", "游戏文件", ".nup 文件", "romfs 和 exefs",
        "也可以将文件或文件夹拖放到此窗口。", "游戏保存位置", "生成", "浏览", "大约需要 45 分钟和 60 GB 空间。",
        "{0} 可用空间 {1} GB。", "正在检查游戏版本...", "Hookshot 支持此版本。", "不支持此版本。Hookshot 需要构建版本 e0f3ccfa。",
        "在此应用旁边找不到 Hookshot 工具。", "找不到 Python。",
        "正在生成", "这需要一些时间,期间电脑仍可正常使用。", "已用时间", "正在读取游戏文件", "正在将游戏转换为电脑代码", "正在编译",
        "正在整理游戏文件夹", "显示详细信息", "隐藏详细信息", "取消", "返回", "打开文件夹", "开始游戏", "你的游戏已准备就绪", "用时",
        "生成已停止", "生成已取消", "你可以从同一文件夹重新开始。", "要停止生成并关闭吗?", "语言",
    };
    static readonly string[] ZhHant =
    {
        "將你自己的 Ocarina of Time 變成可在電腦上啟動的遊戲資料夾。", "遊戲檔案", ".nup 檔案", "romfs 和 exefs",
        "也可以將檔案或資料夾拖放到此視窗。", "遊戲儲存位置", "建置", "瀏覽", "大約需要 45 分鐘和 60 GB 空間。",
        "{0} 可用空間 {1} GB。", "正在檢查遊戲版本...", "Hookshot 支援此版本。", "不支援此版本。Hookshot 需要建置版本 e0f3ccfa。",
        "在此應用程式旁找不到 Hookshot 工具。", "找不到 Python。",
        "正在建置", "這需要一些時間,期間電腦仍可正常使用。", "已用時間", "正在讀取遊戲檔案", "正在將遊戲轉換為電腦程式碼", "正在編譯",
        "正在整理遊戲資料夾", "顯示詳細資訊", "隱藏詳細資訊", "取消", "返回", "開啟資料夾", "開始遊戲", "你的遊戲已準備就緒", "用時",
        "建置已停止", "建置已取消", "你可以從同一資料夾重新開始。", "要停止建置並關閉嗎?", "語言",
    };

    public static readonly Language[] All =
    {
        new("en-US", "English (US)", En), new("en-GB", "English (UK)", En), new("fr", "Français", Fr), new("fr-CA", "Français (Canada)", Fr),
        new("de", "Deutsch", De), new("it", "Italiano", It), new("es", "Español (España)", Es), new("es-419", "Español (Latinoamérica)", Es),
        new("pt-BR", "Português (Brasil)", Pt), new("nl", "Nederlands", Nl), new("pl", "Polski", Pl), new("ru", "Русский", Ru),
        new("ja", "日本語", Ja), new("ko", "한국어", Ko), new("zh-Hans", "简体中文", ZhHans), new("zh-Hant", "繁體中文", ZhHant),
    };

    /// The language closest to Windows' own: the exact one, then the same family, else English (US).
    public static Language Default()
    {
        var c = CultureInfo.CurrentUICulture;
        var name = c.Name;
        var pick = All.FirstOrDefault(l => l.Code.Equals(name, StringComparison.OrdinalIgnoreCase));
        if (pick != null) return pick;
        if (name.StartsWith("zh")) return All.First(l => l.Code == (name.Contains("TW") || name.Contains("HK") || name.Contains("Hant") ? "zh-Hant" : "zh-Hans"));
        if (name.StartsWith("es-") && name != "es-ES") return All.First(l => l.Code == "es-419");
        if (name.StartsWith("pt")) return All.First(l => l.Code == "pt-BR");
        return All.FirstOrDefault(l => l.Code == c.TwoLetterISOLanguageName) ?? All[0];
    }
}
