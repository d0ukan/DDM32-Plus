# DDM32 Plus telsiz kullanım kılavuzu

Baofeng DM-32UV üzerinde DDM32 Plus firmware'inin kısa, pratik kılavuzu. Menü adları telsizde göründüğü gibi
yazıldı (telsiz varsayılan olarak Türkçe; **Ayarlar > Dil/Language** ile İngilizceye geçer). Kurulum ve veri paketi
yükleme Windows programıyla yapılır: [`win/docs/kullanim.md`](../win/docs/kullanim.md). Ayrıntılı teknik notlar:
[`docs/plus.md`](plus.md); upstream'in İngilizce kılavuzu `docs/user/` (RadioFoundry).

> Yayın (TX) yalnızca lisansınızın izin verdiği frekanslarda yapılır. Acil durum haberleşmesi için bu firmware'e
> güvenmeyin. Birçok özellik henüz yalnızca simülatörde denendi ([`plus-radio-test.md`](plus-radio-test.md)).

## 1. Tuşlar

Her tuş her ekranda aynı türden işi yapar:

| Tuş | Basınca | Basılı tutunca |
|---|---|---|
| **OK** (yeşil Menü) | Ana ekranda menüyü (hub) açar; listelerde seçer | Kalıcı yapar: "Kanala kaydedildi" |
| **Geri** (kırmızı) | Bir üst seviye; ana ekranda geçici bir değişikliği geri alır | Her yerden ana ekrana |
| **Yukarı / Aşağı**, düğme | Odağı taşır; ana ekranda kanal (VFO'da adım) | Tekrarlar |
| **Sol / Sağ** | Ana ekranda önceki / sonraki bölge; çoklu izlemede önceki / sonraki harf | |
| **\*** | Hızlı ayarlar (güç, susturma, ton, mod, tarama, ...) | Tuş kilidi aç / kapa |
| **#** | Odaktaki şeyin eylemleri; ana ekranda son arayanın | DMR'de konuşma hedefi seçici |
| **0-9** | Ana ekranda Bul (kanal no, frekans, ad, ID); listelerde numaralı satır veya harfle süzme | **4 basılı tut** (kanalda ve çift dinlemede, VFO'da değil): ISS izleme; alt satırda "4 ISS" yazar |
| **SK1** (yan) | FM: susturmayı açar; DMR: dijital dinleme aç / kapa | |
| **SK2** (yan) | Dual watch / çoklu izleme: sonraki harf | İzlemeyi aç / kapa |
| **PTT** | Gönder (listede odaktaki kişiye / gruba bir kez) | |

Ekranın alt satırı ipucu satırıdır: o ekranda yakındaki tuşların ne yaptığını söyler; sağ ucu **Geri**'nin işidir.
Hub'da numaralar doğrudan açar: **OK 1** Kişiler, **2** Mesajlar, **3** Son çağrılar, **4** Bölgeler, **5** Kanallar,
**6** Araçlar, **7** Ayarlar.

Operasyon sırasında yapılan değişiklikler (güç, ton, slot, renk kodu, konuşma hedefi, ...) **geçicidir**: kanal
değişince ya da ana ekranda **Geri** ile kalkar. Kanala yazmak için ana ekranda veya hızlı ayarlarda **OK**'yi basılı
tutun. VFO kendini kendiliğinden saklar.

## 2. Ana ekran

Üstte durum şeridi (IDLE / RX / TX, mod, pil), ortada kanal numarası ve adı, konuşma hedefi, mod çipleri ve
**kanalın frekansı** (rölelerde RX/TX), altta etkinlik (arayan, sinyal) ve ipucu satırı.

- **Ayarlar > Ekran > Büyük yazı**: ana ekranda yalnız kanal ve konuşma hedefi, iki kata kadar büyük.
- **Ayarlar > Ekran**: tema (koyu / açık / yüksek kontrast), vurgu rengi, arka ışık.
- Bir rakam yazmak **Bul**'u açar: "12" bölgenin 12. kanalı, "433500" VFO'yu 433.500 MHz'e ayarlar, harfler kanal,
  kişi ve ayar adlarını bulur.

## 3. Bölgeler ve kanallar

- **Bölgeler** (OK 4): bölge seç, **Yeni Bölge Oluştur**, bir bölgenin kanal listesinde ilk satır kanal ekler.
- **Kanallar** (OK 5): bir kanalda **OK** kanal düzenleyiciyi açar (ad, frekans, mod, tonlar, DMR / P25
  ayarları). Bir bölgede **#** kanal menüsü: Bölgeden çıkar, Öncelikli yap, ...
- **Kanalı sil**: kanal düzenleyicinin son satırı ya da bölgenin kanal menüsünün son girişi. Önce sorar ("Kanal
  silinsin mi?", kaç bölgeden çıkacağı, "Geri alınamaz."); **Hayır** odaktadır. **Evet**: kanal bütün bölgelerden
  çıkar ve kaydı silinir. Kullanılan kanal silinmez: önce başka bir kanala geçin.
- **VFO**: hızlı ayarlarda kanalı VFO'ya kopyalayın ya da Bul'a frekans yazın. VFO'da her mod seçilebilir
  (FM / DMR / P25 / YSF / NXDN / dPMR / AM), Any değerleri dahil.

## 4. Dual watch ve çoklu izleme

**Ayarlar > Konuşma ve dinleme > İzlenen kanallar**: 2 (klasik dual watch) ile 6 arası. İzlemeyi **SK2**'yi basılı
tutarak, **\* 7** ile ya da **SK2**'ye basarak başlatın.

- Ana ekranda her kanal bir satır: **A-F** harfleri. A bulunduğunuz kanal, B-F bölgedeki sonraki kanallar. Her harf
  bir kanal **ya da kendi VFO'su** olabilir.
- **SK2** veya **# 1** sonraki harfi seçer (PTT seçili harfte gönderir); **Sol / Sağ** önceki / sonraki.
- **#**: 1 Konuş: (sonraki harf), 2-3 ve 6-9 bir harfin kanalını değiştir, 4 Arayan, 5 Durdur.
- Alt satır: "Alınıyor: C" sinyal çubuğuyla, "KONUŞUYOR: C" ya da "Döngü 0.9 s" (bir turun süresi; P25 kanalı varsa
  "P25 +3.5 s" eklenir: P25 döngüyü yavaşlatır).
- Durdurmak: **Geri** iki kez, **\* 7** ya da **SK2** basılı. Telsiz seçili kanalda kalır.

## 5. Modlar

| Mod | Ne yapar | Bilinmesi gerekenler |
|---|---|---|
| **FM / AM** | Alma ve gönderme (AM yalnız alma: hava bandı 108-136 MHz) | CTCSS / DCS tonları; Araçlar > Ton bulucu bilinmeyen tonu bulur |
| **DMR** | Alma ve gönderme, Tier I/II | Renk kodu, slot ve RX grubu **Any** olabilir; **SK1** dijital dinleme (her çağrı); SMS, acil çağrı, roaming |
| **P25** | Faz 1 alma ve gönderme | NAC **desimal** yazılır: "659 ($293)"; **#** RX'te Any, TX'te "RX ile aynı". Ayrı RX / TX NAC; Any talkgroup |
| **YSF** | DN ve VW alma; başlıktan çağrı işareti | VW gönderme deneysel, varsayılan kapalı |
| **NXDN48** | Yalnız alma | |
| **dPMR** | Yalnız alma | |

Kanal düzenleyicide **Mod** satırı modu seçer; her modun kendi satırları açılır.

## 6. Arama ve tarama

- **Tarama** (\* hızlı ayarlar veya Araçlar > Tarama): bölgeyi tarar, P25 kanallarındaki kısa çağrıları da yakalar;
  öncelikli kanal ve geçici atlama.
- **Araçlar > P25 arama / DMR arama**: bir aralıkta P25 veya DMR sinyali arar, bulduklarını listeler (NAC / renk
  kodu ile). **Sol / Sağ** hazır aralıklar (kanalın çevresi, 430-440, 144-146 MHz) ve kendi aralığınız.
  **\*** aralık düzenleyiciyi açar ("Arama aralığı, MHz"): Alt, Üst ve Adım; rakamlar doğrudan Alt'ı yazar,
  **Sol / Sağ** bir adım, basılı tutunca 1 MHz. Aralık tek bantta olmalı.
- **Spektrum** (Araçlar): çubuklar, şelale ve 3D şelale; close call.

## 7. Harita, APRS, havalimanları

- **Araçlar > Harita**: çevrimdışı Türkiye haritası (81 il, 973 ilçe), röleler, duyulan APRS istasyonları ve izi.
- **APRS** (Araçlar > APRS): beacon (elle, otomatik, SmartBeaconing), **Mic-E** (Ayarlar > APRS > Beacon biçimi),
  mesajlar, SOS. **Duyulan istasyonlar** listesinde **OK** istasyon sayfasını açar: sembol, uzaklık ve yön, konum
  ve lokatör, irtifa, hız, yorum, ne zaman ve nasıl duyulduğu, son mesajlar; oradan **Hedefe git** ve harita.
- **Röleler / Otomatik röle**: 252 hazır röle, il il; tek tuşla VFO'ya. Yolculukta daha yakın röle önerilir.
- **Havalimanları**: önce "Hepsi" ve havalimanı olan iller (plaka sırasıyla); **OK** o ilin listesi, en yakın
  önce; bir havalimanında **OK** frekansları, bir frekansta **OK** VFO'yu AM'e ayarlar, **#** hepsini
  birlikte izler (hava izleme: meydanın frekansları çoklu izlemede). Frekanslar hava bandı veri paketinden gelir.
- **Deniz VHF**: kanal tablosu, yalnız alma. **Uydular**, **Güneş ve Ay**, **Hedefe git**, **GPS** de Araçlar'dadır.

## 8. Son çağrılar ve DOTA Aktarım

- **Araçlar > Son çağrı**: her moddan ve izlenen her kanaldan son 8 çağrı ("B DMR Ahmet", "TG2862 2dk"). En yeni P25
  çağrısında **OK** onu yeniden çalar (P25 kanalındayken).
- **Araçlar > DOTA Aktarım**: kanalları, bölgeleri, tümünü ya da bir veri paketini başka bir DDM32 Plus telsize FM
  veya DMR ile gönderir / alır. İki telsiz aynı frekans ve tonda başlar (varsayılan 433.450 MHz, 88.5 Hz); frekans
  mevcut kanaldan, son kullanılandan ya da elle de seçilebilir. Alan telsiz gelenleri gösterir ve sorar. (Beta: iki
  telsiz arasında henüz denenmedi.)

## 9. Veri paketleri

Veri paketleri bilgisayarınızda kendi indirdiğiniz kaynaklardan oluşturulur ve Windows programıyla (Veri paketleri
sayfası) programlama kablosundan yüklenir:

- **RadioID paketi**: DMR ID'lerinin adları (arayan satırında ad ve çağrı işareti).
- **Hava bandı paketi**: havalimanlarının frekansları (AIP AD 2.18 tablolarından).

Hangi paketin yüklü olduğu **Ayarlar > Hakkında**'da yazar (ad ve tarih). Paket yoksa ilgili ekranlar bunu söyler.
Paketler DOTA ile başka bir telsize de gönderilebilir.

## 10. Çapraz bant (kaydet-ilet)

**Araçlar > Çapraz bant**, iki frekans (A ve B) arasında **DMR** kaydet-ilet röledir: birinde duyulan konuşma bütün
olarak kaydedilir, sonra diğerinde gönderilir. Kurallar (telsiz uygular):

- **Yalnız başındayken**: her başlatmada sorar. Çalışırken ekran kilitlidir; yalnız **Geri** durdurur. Her ekranda
  **CROSS** işareti görünür.
- Yalnız amatör simpleks frekanslar (144-146, 430-440 MHz); röle ve uydu bölümleri, APRS 144.800, PMR446 ve röle
  listesindeki frekanslar reddedilir. A ve B aynı renk kodu ve slotu kullanır.
- Bir konuşma en çok yaklaşık 38 saniye kaydedilir; daha uzunu kesilir. Her 10 dakikada çağrı işaretiniz CW ile
  gönderilir (telsiz adı çağrı işareti olmalı).
- FM ve P25 çapraz bant yoktur (bellek ve tek alıcı yetmez).

## 11. Tanılama ve diğer araçlar

- **Araçlar > Tanılama**: tuş ve radyo durumu, DMR alım sesi sayaçları (sorun bildirirken ekran görüntüsü işe yarar).
- **Duyulanlar**, **Son çağrılar** (OK 3; **\*** bir QSO'yu işaretler), **Pil** (grafik ve kalan süre),
  **Saat**, **Fox hunt**, **CW beacon / CW çözücü**, **Pager**, **ACARS**, **SSTV**, **DTMF / SelCall**,
  **Lone worker**, **SOS tuşu**, **Hotspot** (programlama kablosu üzerinden MMDVM).
- **Ayarlar > Hakkında**: sürüm, röle verisi ve veri paketleri.

Sorun bildirimi: [github.com/d0ukan/DDM32-Plus/issues](https://github.com/d0ukan/DDM32-Plus/issues) (upstream
projelere değil).
