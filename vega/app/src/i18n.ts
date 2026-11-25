/**
 * UI text in the Android app's 10 languages (strings.xml under app/src/main/res, same keys).
 * hint_apple leaves out music apps: audio-only AirPlay isn't supported on Vega yet.
 * {1}, {2}: arguments, as %1$s in the Android strings.
 */
import {I18nManager} from '@amazon-devices/react-native-kepler';

const en = {
  home_title: 'Ready to mirror',
  home_ip: 'TV address: {1}',
  hint_apple:
    'Control Center → Screen Mirroring, or the AirPlay icon in a video app',
  hint_choose: 'Then choose “{1}”. Same Wi-Fi as this TV.',
  zoom_fit: 'Fit · press OK to fill the screen',
  zoom_fill: 'Fill · press OK to show the whole picture',
  update_title: 'Update Screenatic',
  update_body:
    'This version is no longer supported. Update to the latest version to keep casting to this TV.',
  update_button: 'Open store',
  media_failed: "Can't play this media. Try another file or format.",
  hint_apple_label: 'iPhone · iPad · Mac',
};
type Key = keyof typeof en;

const translations: Record<string, Partial<Record<Key, string>>> = {
  de: {
    home_title: 'Bereit zum Spiegeln',
    home_ip: 'TV-Adresse: {1}',
    hint_apple:
      'Kontrollzentrum → Bildschirmsynchronisierung oder das AirPlay-Symbol in einer Video-App',
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
    home_title: 'Listo para duplicar',
    home_ip: 'Dirección del televisor: {1}',
    hint_apple:
      'Centro de control → Duplicar pantalla, o el icono de AirPlay en una app de vídeo',
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
    home_title: 'Prêt pour la recopie',
    home_ip: 'Adresse du téléviseur : {1}',
    hint_apple:
      "Centre de contrôle → Recopie de l'écran, ou l'icône AirPlay dans une app vidéo",
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
    home_title: 'Pronto per la duplicazione',
    home_ip: 'Indirizzo della TV: {1}',
    hint_apple:
      "Centro di Controllo → Duplica schermo, oppure l'icona AirPlay in un'app video",
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
    home_title: 'ミラーリングの準備完了',
    home_ip: 'テレビのアドレス：{1}',
    hint_apple:
      'コントロールセンター → 画面ミラーリング、または動画アプリの AirPlay アイコン',
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
    home_title: '미러링 준비 완료',
    home_ip: 'TV 주소: {1}',
    hint_apple: '제어 센터 → 화면 미러링, 또는 동영상 앱의 AirPlay 아이콘',
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
    home_title: 'Pronto para espelhar',
    home_ip: 'Endereço da TV: {1}',
    hint_apple:
      'Central de Controle → Espelhamento de Tela, ou o ícone do AirPlay em um app de vídeo',
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
    home_title: '准备投屏',
    home_ip: '电视地址：{1}',
    hint_apple: '控制中心 → 屏幕镜像，或视频 App 里的 AirPlay 图标',
    hint_choose: '然后选择“{1}”，需与电视连接同一 Wi-Fi。',
    zoom_fit: '适应屏幕 · 按 OK 切换为填充',
    zoom_fill: '填充屏幕 · 按 OK 切换为完整显示',
    update_title: '请更新 Screenatic',
    update_body: '当前版本已停止支持，请更新到最新版本后继续投屏。',
    update_button: '打开应用商店',
    media_failed: '无法播放此媒体，请换一个文件或格式试试。',
  },
  'zh-TW': {
    home_title: '準備投放',
    home_ip: '電視位址：{1}',
    hint_apple: '控制中心 → 螢幕鏡像輸出，或影片 App 裡的 AirPlay 圖示',
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
