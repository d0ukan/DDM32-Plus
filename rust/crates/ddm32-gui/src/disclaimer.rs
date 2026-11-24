// SPDX-License-Identifier: GPL-3.0-or-later
//! The disclaimer (the C# program's DisclaimerDialog and Resources/Strings*.resx Disc_*, from pc/ddm32_app/
//! disclaimer.py): shown at the first start and whenever its text changed, and from Help; every radio step stays off
//! until it is accepted. The acceptance is the C# program's settings.json `disclaimer` object (`version`, `accepted`,
//! `accepted_utc`), and the version is computed exactly as the C# AppState.DisclaimerVersion() does, from the same
//! texts: a user who accepted in the C# program is not asked again here, and the other way round.

/// (C# resource key, Turkish, English), in the order DisclaimerVersion() hashes them. The texts are the resx values
/// byte for byte (`**` marks bold there); changing a single character asks every user again.
pub const BODY: [(&str, &str, &str); 8] = [
    ("Disc_Title", "⚠ SORUMLULUK REDDİ — OKUMADAN KULLANMAYIN", "⚠ DISCLAIMER — READ BEFORE USING"),
    ("Disc_Intro", "**DDM32 Plus** **resmî olmayan, deneysel** bir firmware ve programlama aracıdır. Baofeng, RadioFoundry veya Open25-DM32 ile bağlantılı değildir ve onlar tarafından onaylanmamıştır.", "**DDM32 Plus** is **unofficial, experimental** firmware and programming software. It is not affiliated with or endorsed by Baofeng, RadioFoundry or Open25-DM32."),
    ("Disc_B1", "Telsizinize firmware yüklemek (flashlamak) **telsizinizi kalıcı olarak kullanılamaz hâle getirebilir**, kalibrasyon verilerini, kanal kayıtlarını ve garantinizi kaybettirebilir.", "Flashing firmware **can permanently brick your radio**, erase calibration and channel data, and void your warranty."),
    ("Disc_B2", "Bu yazılım **\"OLDUĞU GİBİ\"**, hiçbir açık veya örtülü garanti olmaksızın sunulur. Çalışacağı, hatasız olduğu veya belirli bir amaca uygun olduğu garanti **edilmez**.", "This software is provided **\"AS IS\", without warranty of any kind**, express or implied, including merchantability or fitness for a particular purpose."),
    ("Disc_B3", "Yükleme, kurulum ve kullanımın **tüm riski ve sorumluluğu size aittir.** Geliştiriciler (d0ukan / TA2DOU) ve katkıda bulunanlar; telsizde, bilgisayarda veya başka ekipmanda oluşabilecek **hiçbir arıza, hasar, veri kaybı, doğrudan veya dolaylı zarardan sorumlu tutulamaz.**", "**You flash, install and use it entirely at your own risk and responsibility.** The authors (d0ukan / TA2DOU) and contributors accept **no liability for any malfunction, damage, data loss, or direct or indirect loss** to radios, computers or any other equipment."),
    ("Disc_B4", "Yayın yapmak (TX) yalnızca **geçerli bir lisansla ve izinli frekanslarda** yasaldır. Telsiz mevzuatına uymak tamamen kullanıcının sorumluluğundadır. Acil durum ve can güvenliği haberleşmesi için bu firmware'e **güvenmeyin.**", "Transmitting is legal **only with a valid licence on authorised frequencies**; complying with radio regulations is solely your responsibility. **Do not rely** on this firmware for emergency or safety-of-life communication."),
    ("Disc_B5", "Yüklemeden önce mutlaka **tam yedek** alın ve yedeği güvenli bir yerde saklayın.", "Always make and keep a **full backup** before flashing."),
    ("Disc_Outro", "**Bu yazılımı indirerek, yükleyerek veya kullanarak bu koşulları okuduğunuzu, anladığınızı ve kabul ettiğinizi beyan etmiş olursunuz. Kabul etmiyorsanız kullanmayın.**", "**By downloading, installing or using this software you confirm that you have read, understood and accepted these terms. If you do not accept them, do not use it.**"),
];

/// The version of today's text: the first 16 hex digits of SHA-256 over every Turkish text, then every English one
/// (UTF-8), as AppState.DisclaimerVersion() in the C# program.
pub fn version() -> String {
    let mut text = String::new();
    for (_, tr, _) in BODY {
        text.push_str(tr);
    }
    for (_, _, en) in BODY {
        text.push_str(en);
    }
    ddm32_core::releases::signed_manifest::sha256_hex(text.as_bytes())[..16].to_string()
}

/// One text of the disclaimer in a language ("en", else Turkish).
pub fn text(lang: &str, key: &str) -> &'static str {
    BODY.iter().find(|(k, _, _)| *k == key).map(|(_, tr, en)| if lang == "en" { *en } else { *tr }).unwrap_or("")
}

/// A paragraph as the window shows it: the `**` bold marks taken out; `true` when the whole paragraph was bold (the
/// closing sentence), which is then drawn in bold.
pub fn plain(text: &str) -> (String, bool) {
    let t = text.trim();
    let all_bold = t.starts_with("**") && t.ends_with("**") && t.len() > 4 && !t[2..t.len() - 2].contains("**");
    (t.replace("**", ""), all_bold)
}

/// What the dialog shows in a language: (title, intro, the five points, the closing sentence, closing in bold).
pub struct DisclaimerView {
    pub title: String,
    pub intro: String,
    pub points: Vec<String>,
    pub outro: String,
    pub outro_bold: bool,
}

pub fn view(lang: &str) -> DisclaimerView {
    let p = |k: &str| plain(text(lang, k)).0;
    let (outro, outro_bold) = plain(text(lang, "Disc_Outro"));
    DisclaimerView {
        title: p("Disc_Title"),
        intro: p("Disc_Intro"),
        points: ["Disc_B1", "Disc_B2", "Disc_B3", "Disc_B4", "Disc_B5"].iter().map(|k| p(k)).collect(),
        outro,
        outro_bold,
    }
}
