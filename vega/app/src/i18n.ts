/**
 * UI text in the Android app's 10 languages (strings.xml under app/src/main/res, same keys).
 * hint_other / now_playing_*: DLNA (Android phones, Windows).
 * {1}, {2}: arguments, as %1$s in the Android strings.
 */
import {I18nManager} from '@amazon-devices/react-native-kepler';

const en = {
  home_title: 'Ready to mirror',
  home_ip: 'TV address: {1}',
  hint_apple:
    'Control Center → Screen Mirroring, or the AirPlay icon in a video or music app',
  hint_choose: 'Then choose “{1}”. Same Wi-Fi as this TV.',
  zoom_fit: 'Fit · press OK to fill the screen',
  zoom_fill: 'Fill · press OK to show the whole picture',
  update_title: 'Update Screenatic',
  update_body:
    'This version is no longer supported. Update to the latest version to keep casting to this TV.',
  update_button: 'Open store',
  media_failed: "Can't play this media. Try another file or format.",
  hint_apple_label: 'iPhone · iPad · Mac',
  hint_other_label: 'Android · Windows',
  hint_other:
    'Cast (DLNA) in a video, music or photo app, or Windows “Cast to Device”',
  now_playing_from: 'Playing from {1}',
  now_playing_unknown: 'Unknown track',
  // Cast time (ads for time), as on Android.
  time_left: 'Cast time left: {1}',
  watch_ad: 'Watch an ad · +{1} min',
  grace_banner: 'Cast time is up. Casting stops in {1} — watch a short ad to keep going.',
  reward_cap_reached: 'Daily ad limit reached. More cast time tomorrow.',
  time_dev_unlimited: 'Dev build · unlimited cast time',
  dev_unlimited_on: 'Dev: unlimited time · ON',
  dev_unlimited_off: 'Dev: unlimited time · OFF',
  dev_reset_balance: 'Dev: reset balance to 0',
};
type Key = keyof typeof en;

const translations: Record<string, Partial<Record<Key, string>>> = {
  de: {
    time_left: 'Verbleibende Streamingzeit: {1}',
    watch_ad: 'Werbung ansehen · +{1} Min.',
    grace_banner: 'Die Streamingzeit ist abgelaufen. Die Übertragung endet in {1} – sieh dir eine kurze Werbung an, um weiterzumachen.',
    reward_cap_reached: 'Tageslimit für Werbung erreicht. Morgen gibt es mehr Zeit.',
    time_dev_unlimited: 'Entwicklerversion · unbegrenzte Zeit',
    dev_unlimited_on: 'Dev: unbegrenzte Zeit · AN',
    dev_unlimited_off: 'Dev: unbegrenzte Zeit · AUS',
    dev_reset_balance: 'Dev: Guthaben auf 0',
    hint_other:
      'Streamen (DLNA) in einer Video-, Musik- oder Foto-App oder „Auf Gerät übertragen“ in Windows',
    now_playing_from: 'Wiedergabe von {1}',
    now_playing_unknown: 'Unbekannter Titel',
    home_title: 'Bereit zum Spiegeln',
    home_ip: 'TV-Adresse: {1}',
    hint_apple:
      'Kontrollzentrum → Bildschirmsynchronisierung oder das AirPlay-Symbol in einer Video- oder Musik-App',
    hint_choose: 'Dann „{1}“ auswählen. Gleiches WLAN wie der Fernseher.',
    zoom_fit: 'Einpassen · OK drücken, um den Bildschirm zu füllen',
    zoom_fill: 'Füllen · OK drücken, um das ganze Bild zu zeigen',
    update_title: 'Screenatic aktualisieren',
    update_body:
      'Diese Version wird nicht mehr unterstützt. Aktualisiere auf die neueste Version, um weiter auf diesen Fernseher zu streamen.',
    update_button: 'Store öffnen',
    media_failed:
      'Dieses Medium kann nicht wiedergegeben werden. Versuche eine andere Datei oder ein anderes Format.',
  },
  es: {
    time_left: 'Tiempo de transmisión restante: {1}',
    watch_ad: 'Ver un anuncio · +{1} min',
    grace_banner: 'Se acabó el tiempo de transmisión. Se detendrá en {1}: mira un anuncio corto para continuar.',
    reward_cap_reached: 'Límite diario de anuncios alcanzado. Mañana tendrás más tiempo.',
    time_dev_unlimited: 'Versión de desarrollo · tiempo ilimitado',
    dev_unlimited_on: 'Dev: tiempo ilimitado · SÍ',
    dev_unlimited_off: 'Dev: tiempo ilimitado · NO',
    dev_reset_balance: 'Dev: poner saldo a 0',
    hint_other:
      'Enviar (DLNA) en una app de vídeo, música o fotos, o «Transmitir a dispositivo» en Windows',
    now_playing_from: 'Reproduciendo desde {1}',
    now_playing_unknown: 'Pista desconocida',
    home_title: 'Listo para duplicar',
    home_ip: 'Dirección del televisor: {1}',
    hint_apple:
      'Centro de control → Duplicar pantalla, o el icono de AirPlay en una app de vídeo o música',
    hint_choose:
      'Después elige «{1}». Usa la misma red Wi-Fi que el televisor.',
    zoom_fit: 'Ajustar · pulsa OK para llenar la pantalla',
    zoom_fill: 'Llenar · pulsa OK para ver la imagen completa',
    update_title: 'Actualiza Screenatic',
    update_body:
      'Esta versión ya no es compatible. Actualiza a la última versión para seguir transmitiendo a este televisor.',
    update_button: 'Abrir tienda',
    media_failed:
      'No se puede reproducir este contenido. Prueba con otro archivo o formato.',
  },
  fr: {
    time_left: 'Temps de diffusion restant : {1}',
    watch_ad: 'Regarder une pub · +{1} min',
    grace_banner: 'Temps de diffusion écoulé. Arrêt dans {1} — regardez une courte pub pour continuer.',
    reward_cap_reached: 'Limite quotidienne de pubs atteinte. Plus de temps demain.',
    time_dev_unlimited: 'Version de développement · temps illimité',
    dev_unlimited_on: 'Dev : temps illimité · OUI',
    dev_unlimited_off: 'Dev : temps illimité · NON',
    dev_reset_balance: 'Dev : remettre le solde à 0',
    hint_other:
      'Caster (DLNA) dans une app vidéo, musique ou photos, ou « Caster sur un appareil » dans Windows',
    now_playing_from: 'Lecture depuis {1}',
    now_playing_unknown: 'Titre inconnu',
    home_title: 'Prêt pour la recopie',
    home_ip: 'Adresse du téléviseur : {1}',
    hint_apple:
      "Centre de contrôle → Recopie de l'écran, ou l'icône AirPlay dans une app vidéo ou musique",
    hint_choose:
      'Puis choisissez « {1} ». Même réseau Wi-Fi que le téléviseur.',
    zoom_fit: "Ajuster · appuyez sur OK pour remplir l'écran",
    zoom_fill: "Remplir · appuyez sur OK pour voir toute l'image",
    update_title: 'Mettez à jour Screenatic',
    update_body:
      "Cette version n'est plus prise en charge. Installez la dernière version pour continuer à diffuser sur ce téléviseur.",
    update_button: 'Ouvrir la boutique',
    media_failed:
      'Impossible de lire ce média. Essayez un autre fichier ou format.',
  },
  it: {
    time_left: 'Tempo di trasmissione rimanente: {1}',
    watch_ad: 'Guarda un annuncio · +{1} min',
    grace_banner: 'Il tempo di trasmissione è finito. La trasmissione si interrompe tra {1}: guarda un breve annuncio per continuare.',
    reward_cap_reached: 'Limite giornaliero di annunci raggiunto. Altro tempo domani.',
    time_dev_unlimited: 'Versione di sviluppo · tempo illimitato',
    dev_unlimited_on: 'Dev: tempo illimitato · SÌ',
    dev_unlimited_off: 'Dev: tempo illimitato · NO',
    dev_reset_balance: 'Dev: azzera saldo',
    hint_other:
      "Trasmetti (DLNA) in un'app video, musicale o di foto, oppure «Trasmetti a dispositivo» in Windows",
    now_playing_from: 'In riproduzione da {1}',
    now_playing_unknown: 'Brano sconosciuto',
    home_title: 'Pronto per la duplicazione',
    home_ip: 'Indirizzo della TV: {1}',
    hint_apple:
      "Centro di Controllo → Duplica schermo, oppure l'icona AirPlay in un'app video o musicale",
    hint_choose: 'Poi scegli «{1}». Stessa rete Wi-Fi della TV.',
    zoom_fit: 'Adatta · premi OK per riempire lo schermo',
    zoom_fill: "Riempi · premi OK per vedere tutta l'immagine",
    update_title: 'Aggiorna Screenatic',
    update_body:
      "Questa versione non è più supportata. Aggiorna all'ultima versione per continuare a trasmettere su questa TV.",
    update_button: 'Apri lo store',
    media_failed:
      'Impossibile riprodurre questo contenuto. Prova un altro file o formato.',
  },
  ja: {
    time_left: '残りのキャスト時間：{1}',
    watch_ad: '広告を見る · +{1} 分',
    grace_banner: 'キャスト時間がなくなりました。{1} 後に停止します。短い広告を見ると続けられます。',
    reward_cap_reached: '本日の広告視聴の上限に達しました。明日またキャスト時間を受け取れます。',
    time_dev_unlimited: '開発版 · キャスト時間無制限',
    dev_unlimited_on: '開発：無制限 · オン',
    dev_unlimited_off: '開発：無制限 · オフ',
    dev_reset_balance: '開発：残り時間を 0 に',
    hint_other:
      '動画・音楽・写真アプリの「キャスト」（DLNA）、または Windows の「デバイス キャスト」',
    now_playing_from: '{1} から再生中',
    now_playing_unknown: '不明なトラック',
    home_title: 'ミラーリングの準備完了',
    home_ip: 'テレビのアドレス：{1}',
    hint_apple:
      'コントロールセンター → 画面ミラーリング、または動画・音楽アプリの AirPlay アイコン',
    hint_choose:
      '「{1}」を選択してください。テレビと同じ Wi-Fi に接続してください。',
    zoom_fit: 'フィット · OK で画面いっぱいに表示',
    zoom_fill: 'フィル · OK で全体を表示',
    update_title: 'Screenatic をアップデート',
    update_body:
      'このバージョンはサポートが終了しました。キャストを続けるには最新版にアップデートしてください。',
    update_button: 'ストアを開く',
    media_failed:
      'このメディアは再生できません。別のファイルや形式をお試しください。',
  },
  ko: {
    time_left: '남은 캐스트 시간: {1}',
    watch_ad: '광고 보기 · +{1}분',
    grace_banner: '캐스트 시간이 끝났습니다. {1} 후 중단됩니다. 짧은 광고를 보면 계속할 수 있습니다.',
    reward_cap_reached: '오늘 광고 시청 한도에 도달했습니다. 내일 다시 캐스트 시간을 받을 수 있습니다.',
    time_dev_unlimited: '개발 버전 · 캐스트 시간 무제한',
    dev_unlimited_on: '개발: 무제한 · 켜짐',
    dev_unlimited_off: '개발: 무제한 · 꺼짐',
    dev_reset_balance: '개발: 남은 시간 0으로',
    hint_other:
      "동영상·음악·사진 앱의 '캐스트'(DLNA) 또는 Windows의 '디바이스로 캐스트'",
    now_playing_from: '{1}에서 재생 중',
    now_playing_unknown: '알 수 없는 트랙',
    home_title: '미러링 준비 완료',
    home_ip: 'TV 주소: {1}',
    hint_apple: '제어 센터 → 화면 미러링, 또는 동영상·음악 앱의 AirPlay 아이콘',
    hint_choose:
      "그런 다음 '{1}'을(를) 선택하세요. TV와 같은 Wi-Fi에 연결되어 있어야 합니다.",
    zoom_fit: '맞춤 · OK를 눌러 화면 채우기',
    zoom_fill: '채우기 · OK를 눌러 전체 화면 보기',
    update_title: 'Screenatic 업데이트',
    update_body:
      '이 버전은 더 이상 지원되지 않습니다. 계속 캐스트하려면 최신 버전으로 업데이트하세요.',
    update_button: '스토어 열기',
    media_failed:
      '이 미디어를 재생할 수 없습니다. 다른 파일이나 형식을 사용해 보세요.',
  },
  'pt-BR': {
    time_left: 'Tempo de transmissão restante: {1}',
    watch_ad: 'Assistir a um anúncio · +{1} min',
    grace_banner: 'O tempo de transmissão acabou. A transmissão para em {1} — assista a um anúncio curto para continuar.',
    reward_cap_reached: 'Limite diário de anúncios atingido. Mais tempo amanhã.',
    time_dev_unlimited: 'Versão de desenvolvimento · tempo ilimitado',
    dev_unlimited_on: 'Dev: tempo ilimitado · LIGADO',
    dev_unlimited_off: 'Dev: tempo ilimitado · DESLIGADO',
    dev_reset_balance: 'Dev: zerar saldo',
    hint_other:
      'Transmitir (DLNA) em um app de vídeo, música ou fotos, ou “Transmitir para dispositivo” no Windows',
    now_playing_from: 'Tocando de {1}',
    now_playing_unknown: 'Faixa desconhecida',
    home_title: 'Pronto para espelhar',
    home_ip: 'Endereço da TV: {1}',
    hint_apple:
      'Central de Controle → Espelhamento de Tela, ou o ícone do AirPlay em um app de vídeo ou música',
    hint_choose: 'Depois escolha “{1}”. Use a mesma rede Wi-Fi da TV.',
    zoom_fit: 'Ajustar · pressione OK para preencher a tela',
    zoom_fill: 'Preencher · pressione OK para ver a imagem inteira',
    update_title: 'Atualize o Screenatic',
    update_body:
      'Esta versão não é mais compatível. Atualize para a versão mais recente para continuar transmitindo para esta TV.',
    update_button: 'Abrir loja',
    media_failed:
      'Não é possível reproduzir esta mídia. Tente outro arquivo ou formato.',
  },
  'zh-CN': {
    time_left: '剩余投屏时长：{1}',
    watch_ad: '看广告 · +{1} 分钟',
    grace_banner: '投屏时长已用完，{1} 后将断开，看一段短广告即可继续。',
    reward_cap_reached: '今日广告次数已用完，明天可继续领取时长。',
    time_dev_unlimited: '开发版 · 投屏不限时',
    dev_unlimited_on: '开发：不限时 · 开',
    dev_unlimited_off: '开发：不限时 · 关',
    dev_reset_balance: '开发：时长清零',
    hint_other_label: '安卓 · Windows',
    hint_other:
      '视频、音乐、相册 App 里的“投屏”（DLNA），或 Windows 的“投射到设备”',
    now_playing_from: '正在播放 · 来自 {1}',
    now_playing_unknown: '未知曲目',
    home_title: '准备投屏',
    home_ip: '电视地址：{1}',
    hint_apple: '控制中心 → 屏幕镜像，或视频、音乐 App 里的 AirPlay 图标',
    hint_choose: '然后选择“{1}”，需与电视连接同一 Wi-Fi。',
    zoom_fit: '适应屏幕 · 按 OK 切换为填充',
    zoom_fill: '填充屏幕 · 按 OK 切换为完整显示',
    update_title: '请更新 Screenatic',
    update_body: '当前版本已停止支持，请更新到最新版本后继续投屏。',
    update_button: '打开应用商店',
    media_failed: '无法播放此媒体，请换一个文件或格式试试。',
  },
  'zh-TW': {
    time_left: '剩餘投放時間：{1}',
    watch_ad: '觀看廣告 · +{1} 分鐘',
    grace_banner: '投放時間已用完，{1} 後將中斷，觀看一段短廣告即可繼續。',
    reward_cap_reached: '今日廣告次數已達上限，明天再來領取投放時間。',
    time_dev_unlimited: '開發版 · 投放不限時',
    dev_unlimited_on: '開發：不限時 · 開',
    dev_unlimited_off: '開發：不限時 · 關',
    dev_reset_balance: '開發：時間歸零',
    hint_other:
      '影片、音樂、相簿 App 裡的「投放」（DLNA），或 Windows 的「投放到裝置」',
    now_playing_from: '正在播放：來自 {1}',
    now_playing_unknown: '未知曲目',
    home_title: '準備投放',
    home_ip: '電視位址：{1}',
    hint_apple: '控制中心 → 螢幕鏡像輸出，或影片、音樂 App 裡的 AirPlay 圖示',
    hint_choose: '然後選擇「{1}」，需與電視連接同一個 Wi-Fi。',
    zoom_fit: '適應畫面 · 按 OK 填滿螢幕',
    zoom_fill: '填滿 · 按 OK 顯示完整畫面',
    update_title: '請更新 Screenatic',
    update_body: '目前版本已停止支援，請更新到最新版本後繼續投放。',
    update_button: '開啟應用程式商店',
    media_failed: '無法播放此媒體，請換一個檔案或格式試試。',
  },
};

/** The system language, mapped to one of ours ("zh-Hant-TW", "zh_TW", "pt-PT"...). */
export function pickLanguage(locale: string): string {
  const parts = locale.replace(/_/g, '-').split(/[-.@]/);
  const lang = (parts[0] || 'en').toLowerCase();
  const rest = parts.slice(1).map((p) => p.toUpperCase());
  if (lang === 'zh') {
    const traditional = rest.some(
      (p) => p === 'HANT' || p === 'TW' || p === 'HK' || p === 'MO',
    );
    return traditional ? 'zh-TW' : 'zh-CN';
  }
  if (lang === 'pt') return 'pt-BR';
  return translations[lang] ? lang : 'en';
}

function systemLocale(): string {
  try {
    const l = I18nManager.getSystemLocale?.();
    if (l) return l;
  } catch {
    // ignore
  }
  try {
    return Intl.DateTimeFormat().resolvedOptions().locale;
  } catch {
    // ignore
  }
  return 'en-US';
}

export const locale = systemLocale();
export const language = pickLanguage(locale);
const table = translations[language] ?? {};

export function t(key: Key, ...args: (string | number)[]): string {
  let s = table[key] ?? en[key];
  args.forEach((a, i) => {
    s = s.replace(`{${i + 1}}`, String(a));
  });
  return s;
}
