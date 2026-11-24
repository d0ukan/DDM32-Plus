// SPDX-License-Identifier: GPL-3.0-or-later
#![allow(clippy::type_complexity, clippy::assertions_on_constants)]
//! Ed25519, the signed manifest and the version order against the Python code's results (Fixtures/
//! python-results.json) and the C# tests (Ed25519Tests, SignedManifestTests, VersionTests).
mod common;
use common::*;
use ddm32_core::crypto::ed25519;
use ddm32_core::releases::{self, signed_manifest as sm, ReleaseVersion, CURRENT, LEGACY};
use serde_json::{json, Value};

const RFC8032: [(&str, &str, &str, &str); 4] = [
    ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
    ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
    ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42", "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"),
];

fn h(s: &str) -> Vec<u8> {
    hex::decode(s).unwrap()
}

#[test]
fn ed25519_rfc_vectors() {
    for (sk, pk, msg, sig) in RFC8032 {
        assert_eq!(ed25519::secret_to_public(&h(sk)).to_vec(), h(pk));
        assert_eq!(ed25519::sign(&h(sk), &h(msg)).to_vec(), h(sig));
        assert!(ed25519::verify(&h(pk), &h(msg), &h(sig)).unwrap());
    }
}

#[test]
fn ed25519_bad_signatures_are_refused() {
    let (_, pk, msg, sig) = RFC8032[2];
    let (p, m, g) = (h(pk), h(msg), h(sig));
    assert!(!ed25519::verify(&p, &[m.clone(), vec![0]].concat(), &g).unwrap());
    for i in [0, 31, 32, 63] {
        let mut bad = g.clone();
        bad[i] ^= 1;
        assert!(!ed25519::verify(&p, &m, &bad).unwrap());
    }
    assert!(!ed25519::verify(&p, &m, &g[..63]).unwrap());
    assert!(ed25519::verify(&p[..31], &m, &g).is_err());
}

#[test]
fn ed25519_signatures_equal_python() {
    let cases = python()["ed25519"]["sign"].as_array().unwrap();
    for c in cases {
        let (secret, public) = (h(c["secret"].as_str().unwrap()), h(c["public"].as_str().unwrap()));
        let (msg, sig) = (h(c["msg"].as_str().unwrap()), h(c["sig"].as_str().unwrap()));
        assert_eq!(ed25519::secret_to_public(&secret).to_vec(), public);
        assert_eq!(ed25519::sign(&secret, &msg).to_vec(), sig);
        assert!(ed25519::verify(&public, &msg, &sig).unwrap());
    }
    assert_eq!(cases.len(), 36);
}

#[test]
fn ed25519_verify_answers_equal_python() {
    for c in python()["ed25519"]["verify"].as_array().unwrap() {
        let ok = ed25519::verify(
            &h(c["public"].as_str().unwrap()),
            &h(c["msg"].as_str().unwrap()),
            &h(c["sig"].as_str().unwrap()),
        )
        .unwrap_or(false);
        assert_eq!(c["ok"].as_bool().unwrap(), ok, "case '{}'", c["name"]);
    }
}

#[test]
fn embedded_release_key_is_a_curve_point() {
    assert!(ed25519::is_valid_point(&h(releases::PUBLIC_KEY_HEX)));
}

fn check_case(c: &Value) {
    let name = c["name"].as_str().unwrap();
    let data = b64(c["data"].as_str().unwrap());
    let sig = b64(c["sig"].as_str().unwrap());
    let key = c["key"].as_str();
    let schema = if c["schema"] == "legacy" { &LEGACY } else { &CURRENT };
    let r = sm::verify_bytes(&data, &sig, key, Some(schema));
    if c["ok"].as_bool().unwrap() {
        let m = r.unwrap_or_else(|e| panic!("case '{name}': refused: {e}"));
        assert_eq!(c["release"].as_str().unwrap(), m.release, "{name}");
        let want: Vec<&str> = c["versions"].as_array().unwrap().iter().map(|v| v.as_str().unwrap()).collect();
        assert_eq!(want, m.firmware.iter().map(|f| f.version()).collect::<Vec<_>>(), "{name}");
        assert_eq!(c["app"].as_str(), m.app.as_ref().map(|a| a.version.as_str()), "{name}");
    } else {
        match r {
            Ok(_) => panic!("case '{name}': accepted"),
            Err(e) => assert_eq!(c["error"].as_str().unwrap(), e.0, "case '{name}'"),
        }
    }
}

#[test]
fn every_manifest_case_matches_python() {
    let cases = python()["manifests"].as_array().unwrap();
    assert!(cases.len() > 70);
    for c in cases {
        check_case(c);
    }
}

#[test]
fn legacy_manifest_matches_python_with_old_names() {
    for c in python()["legacy"].as_array().unwrap() {
        check_case(c);
    }
}

#[test]
fn constants_equal_python() {
    let k = &python()["constants"];
    assert_eq!(k["format"], CURRENT.format);
    assert_eq!(k["repo"], CURRENT.repo);
    assert_eq!(k["manifest_name"], CURRENT.manifest_name);
    assert_eq!(k["sig_name"], CURRENT.sig_name().as_str());
    assert_eq!(k["url_prefix"], CURRENT.url_prefix().as_str());
    assert_eq!(k["api_url"], releases::API_URL);
    assert_eq!(k["allowed_hosts"], json!(releases::ALLOWED_HOSTS));
    assert_eq!(k["package_files"], json!(releases::PACKAGE_FILES));
    assert_eq!(python()["release_public_key"], releases::PUBLIC_KEY_HEX);
    assert_eq!(python()["release_key_id"], releases::PUBLIC_KEY_ID);
    assert_eq!(releases::PUBLIC_KEY_ID, sm::key_id(&sm::public_key(None).unwrap()));
    assert_eq!(python()["test_public_key"], test_public_hex().as_str());
}

#[test]
fn published_legacy_release_verifies_with_the_embedded_key() {
    let data = std::fs::read(fixture(&["legacy", "ddm25-manifest.json"])).unwrap();
    let sig = std::fs::read(fixture(&["legacy", "ddm25-manifest.json.sig"])).unwrap();
    let m = sm::verify_bytes(&data, &sig, None, Some(&LEGACY)).unwrap();
    assert!(m.schema.is_legacy);
    assert_eq!("v1.0.0-beta.1", m.release);
    assert_eq!(1, m.firmware.len());
    assert_eq!("1.0.0-beta.1", m.firmware[0].version());
    assert_eq!("beta", m.firmware[0].channel);
    assert!(m.app_net.is_none() && m.app_rs.is_empty());
    assert_eq!("Manifest field invalid: format", sm::verify_bytes(&data, &sig, None, None).unwrap_err().0);
}

#[test]
fn app_net_and_app_rs_are_read_and_checked() {
    let mut m = manifest(vec![firmware_entry(&pattern(0x5000, 5), "1.0.0", "stable", None, None)], "v1.0.0");
    m["app_net"] = app_entry("2.0.0-alpha.2", &[1, 2, 3], None);
    m["app_rs"] = json!({"windows-x86_64": app_entry("3.0.0-alpha.1", &[4, 5], None), "linux-x86_64": app_entry("3.0.0-alpha.1", &[6], None)});
    let (data, sig) = signed(&m, None);
    let v = sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap();
    assert_eq!("2.0.0-alpha.2", v.app_net.unwrap().version);
    assert_eq!(2, v.app_rs.len());
    assert_eq!("windows-x86_64", v.app_rs[0].0);
    m["app_net"]["url"] = json!("https://example.com/DDM32Plus-2.0.0-alpha.2.zip");
    let (data, sig) = signed(&m, None);
    assert_eq!(
        "Manifest field invalid: app_net.url",
        sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap_err().0
    );
    m["app_net"] = Value::Null;
    m["app_rs"]["linux-x86_64"]["sha256"] = json!("ab");
    let (data, sig) = signed(&m, None);
    assert_eq!(
        "Manifest field invalid: app_rs.linux-x86_64.sha256",
        sm::verify_bytes(&data, &sig, Some(&test_public_hex()), None).unwrap_err().0
    );
}

#[test]
fn release_gate_needs_the_listed_application() {
    let t = tmp();
    let app = pattern(0x5000, 5);
    write_signed(t.path(), &manifest(vec![firmware_entry(&app, "1.0.0", "stable", None, None)], "v1.0.0"), None);
    let k = test_public_hex();
    assert_eq!("1.0.0", sm::release_gate(&app, t.path(), Some(&k)).unwrap().version());
    assert!(sm::release_gate(&[app.clone(), vec![0]].concat(), t.path(), Some(&k)).is_err());
    assert!(sm::release_gate(&app, t.path(), None).is_err()); // the release key
    assert!(sm::release_gate(&app, &t.path().join("none"), Some(&k)).is_err());
}

#[test]
fn too_large_manifest_is_refused_before_the_signature() {
    let e = sm::verify_bytes(&vec![0u8; (1 << 20) + 1], b"00", Some(&test_public_hex()), None).unwrap_err();
    assert_eq!("Manifest too large or not bytes", e.0);
}

// --- versions ---

#[test]
fn version_python_test_order() {
    let order = ["0.9.0", "1.0.0-beta.1", "1.0.0-beta.2", "1.0.0-beta.10", "1.0.0-rc.1", "1.0.0", "1.0.1", "1.10.0"];
    let mut v: Vec<ReleaseVersion> = order.iter().rev().map(|s| ReleaseVersion::parse(s).unwrap()).collect();
    v.sort();
    assert_eq!(order.to_vec(), v.iter().map(|x| x.text.as_str()).collect::<Vec<_>>());
    assert!(ReleaseVersion::parse("v1.0.0").is_err());
}

#[test]
fn version_same_acceptance_as_python() {
    let v = &python()["versions"];
    for s in v["valid"].as_array().unwrap() {
        assert!(ReleaseVersion::try_parse(s.as_str().unwrap()).is_some(), "{s}");
    }
    for s in v["invalid"].as_array().unwrap() {
        assert!(ReleaseVersion::try_parse(s.as_str().unwrap()).is_none(), "'{s}' should be refused");
    }
}

#[test]
fn version_every_pair_compares_like_python() {
    let v = &python()["versions"];
    let valid: Vec<ReleaseVersion> =
        v["valid"].as_array().unwrap().iter().map(|s| ReleaseVersion::parse(s.as_str().unwrap()).unwrap()).collect();
    let cmp = v["compare"].as_array().unwrap();
    assert!(valid.len() > 25);
    for i in 0..valid.len() {
        for j in 0..valid.len() {
            let want = cmp[i][j].as_i64().unwrap();
            let got = valid[i].cmp(&valid[j]) as i64;
            assert_eq!(want, got, "{:?} vs {:?}", valid[i].text, valid[j].text);
        }
    }
}

#[test]
fn version_operators() {
    let a = ReleaseVersion::parse("1.0.0-beta.1").unwrap();
    let b = ReleaseVersion::parse("1.0.0").unwrap();
    assert!(a < b && b > a && a <= ReleaseVersion::parse("1.0.0-beta.1").unwrap() && b >= a && a != b);
    assert_eq!(ReleaseVersion::parse("1.0.0-007").unwrap(), ReleaseVersion::parse("1.0.0-7").unwrap());
    assert!(a.is_prerelease());
    assert_eq!(Some("beta.1".to_string()), a.prerelease);
}

#[test]
fn program_version_is_in_one_place() {
    assert!(ReleaseVersion::try_parse(ddm32_core::app_info::VERSION).is_some());
    let ws =
        std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../Cargo.toml")).unwrap();
    assert!(ws.contains(&format!("version = \"{}\"", ddm32_core::app_info::VERSION)));
    // the hand-over from the C# program: this program's version sorts above the last C# version
    assert!(
        ReleaseVersion::parse(ddm32_core::app_info::VERSION).unwrap()
            > ReleaseVersion::parse(ddm32_core::app_info::NET_VERSION).unwrap()
    );
    assert_eq!("2.0.0", ddm32_core::app_info::NET_VERSION);
    // while the C# program is still in the tree: NET_VERSION is its AppInfo.Version
    let cs = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../win/src/DDM32Plus.Core/AppInfo.cs");
    if let Ok(text) = std::fs::read_to_string(cs) {
        let want = format!("public const string Version = \"{}\";", ddm32_core::app_info::NET_VERSION);
        assert!(text.contains(&want), "NET_VERSION differs from win/src/DDM32Plus.Core/AppInfo.cs");
    }
}
