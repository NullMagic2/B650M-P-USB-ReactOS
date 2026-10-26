//! Module: lightning.guest_packages
//! Purpose: Resolve dependencies and produce integrity-checked guest setup media.
//! Created: 2026-10-25
//! Architecture: Host-only planning; the guest backend owns privileged installation.
mod bundle;
pub use bundle::prepare_executable;

use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::Read,
    path::{Component, Path},
    process::Command,
};

pub const SCHEMA: u32 = 1;
pub const MAX_PACKAGES: usize = 64;
const MAX_FILES: usize = 256;
const MAX_MANIFEST_BYTES: u64 = 1024 * 1024;
const MAX_PAYLOAD_BYTES: u64 = 512 * 1024 * 1024;
const MAX_TOTAL_PAYLOAD_BYTES: u64 = 1024 * 1024 * 1024;
const MAX_TOTAL_FILES: usize = 512;
const SETUP_NAME: &str = "GuestPackageSetup.exe";

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Catalog {
    pub schema: u32,
    pub packages: Vec<Package>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Package {
    pub id: String,
    pub label: String,
    pub version: String,
    pub os: String,
    pub arch: String,
    #[serde(default)]
    pub requires: Vec<String>,
    #[serde(default)]
    pub required_versions: BTreeMap<String, String>,
    pub license: Payload,
    pub files: Vec<Payload>,
    #[serde(default)]
    pub drivers: Vec<Driver>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Payload {
    pub source: String,
    pub sha256: String,
    #[serde(default)]
    pub destination: String,
    #[serde(default)]
    pub previous_sha256: Vec<String>,
    #[serde(default)]
    pub reboot: bool,
    #[serde(default)]
    pub boot_service: String,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Driver {
    pub inf: String,
    pub hardware_ids: Vec<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Choice {
    pub id: String,
    pub label: String,
    pub os: String,
    pub arch: String,
}

fn identifier(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= 96
        && text
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"-_ .".contains(&c))
        && !text.contains(' ')
        && text != "."
        && text != ".."
        && !text.ends_with('.')
}
pub fn valid_package_id(text: &str) -> bool {
    identifier(text) && !reserved_name(text)
}
fn reserved_name(part: &str) -> bool {
    let stem = part.split('.').next().unwrap_or("").to_ascii_uppercase();
    matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || (stem.len() == 4
            && (stem.starts_with("COM") || stem.starts_with("LPT"))
            && matches!(stem.as_bytes()[3], b'1'..=b'9'))
}
fn relative(text: &str) -> bool {
    !text.is_empty()
        && text.len() < 180
        && text.is_ascii()
        && !text.bytes().any(|byte| byte < 32)
        && !text.contains([
            '\\', ':', '\n', '\r', '%', ';', '=', '"', '*', '?', '<', '>', '|',
        ])
        && Path::new(text)
            .components()
            .all(|c| matches!(c, Component::Normal(_)))
        && text.split('/').all(|part| {
            !part.is_empty()
                && part != "."
                && part != ".."
                && !part.ends_with([' ', '.'])
                && !reserved_name(part)
        })
}
fn hash(text: &str) -> bool {
    text.len() == 64
        && text
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

impl Catalog {
    pub fn load(root: &Path) -> Result<Self> {
        let path = root.join("catalog.json");
        ensure!(
            fs::symlink_metadata(&path)?.file_type().is_file(),
            "Package catalog must be a regular file"
        );
        ensure!(
            fs::metadata(&path)?.len() <= MAX_MANIFEST_BYTES,
            "Package catalog is too large"
        );
        let catalog: Self = serde_json::from_slice(&fs::read(path)?)?;
        catalog.validate()?;
        Ok(catalog)
    }
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema == SCHEMA && self.packages.len() <= MAX_PACKAGES,
            "Unsupported or oversized guest package catalog"
        );
        let mut ids = BTreeSet::new();
        for package in &self.packages {
            ensure!(
                identifier(&package.id)
                    && !reserved_name(&package.id)
                    && ids.insert(package.id.to_ascii_lowercase()),
                "Invalid or duplicate package ID"
            );
            ensure!(
                !package.label.trim().is_empty()
                    && !package.label.chars().any(char::is_control)
                    && identifier(&package.version),
                "Invalid package description/version"
            );
            ensure!(
                matches!(package.os.as_str(), "reactos" | "windows")
                    && matches!(package.arch.as_str(), "i386" | "amd64"),
                "Unsupported guest operating system/architecture"
            );
            ensure!(
                !package.files.is_empty() && package.files.len() <= MAX_FILES,
                "Package payload count is invalid"
            );
            let mut destinations = BTreeSet::new();
            for payload in std::iter::once(&package.license).chain(package.files.iter()) {
                ensure!(
                    relative(&payload.source) && hash(&payload.sha256),
                    "Invalid payload path/hash"
                );
                ensure!(
                    payload.previous_sha256.len() <= MAX_PACKAGES
                        && payload.previous_sha256.iter().all(|h| hash(h)),
                    "Invalid approved previous hash"
                );
                ensure!(
                    payload.boot_service.is_empty()
                        || (identifier(&payload.boot_service)
                            && payload.reboot
                            && !payload.destination.is_empty()),
                    "Boot-service checks require a reboot-verified installed payload"
                );
                if !payload.destination.is_empty() {
                    ensure!(
                        relative(&payload.destination)
                            && destinations.insert(payload.destination.to_ascii_lowercase()),
                        "Unsafe or duplicate guest destination"
                    );
                    ensure!(
                        !payload.reboot || !payload.previous_sha256.is_empty(),
                        "Reboot replacements require approved old binary hashes"
                    );
                }
            }
            for driver in &package.drivers {
                ensure!(
                    relative(&driver.inf) && package.files.iter().any(|p| p.source == driver.inf),
                    "Driver INF is not a pinned payload"
                );
                ensure!(
                    !driver.hardware_ids.is_empty() && driver.hardware_ids.len() <= MAX_FILES,
                    "Missing driver hardware identities"
                );
                ensure!(
                    driver.hardware_ids.iter().all(|id| [
                        "PCI\\", "USB\\", "HID\\", "ACPI\\", "ROOT\\", "SWD\\"
                    ]
                    .iter()
                    .any(|prefix| id.starts_with(prefix))
                        && id.len() < 180
                        && id.is_ascii()
                        && !id.contains(['\n', '\r', ';', '=', '|', '"', '%'])),
                    "Invalid PCI hardware identity"
                );
            }
        }
        for package in &self.packages {
            ensure!(
                package
                    .requires
                    .iter()
                    .all(|id| ids.contains(&id.to_ascii_lowercase())),
                "Missing guest package dependency"
            );
        }
        Ok(())
    }
    pub fn choices(&self) -> Vec<Choice> {
        self.packages
            .iter()
            .map(|p| Choice {
                id: p.id.clone(),
                label: p.label.clone(),
                os: p.os.clone(),
                arch: p.arch.clone(),
            })
            .collect()
    }
    pub fn resolve(&self, selected: &[String]) -> Result<Vec<&Package>> {
        self.validate()?;
        ensure!(
            !selected.is_empty() && selected.len() <= MAX_PACKAGES,
            "Select at least one guest package"
        );
        fn visit<'a>(
            id: &str,
            catalog: &'a Catalog,
            active: &mut BTreeSet<String>,
            done: &mut BTreeSet<String>,
            result: &mut Vec<&'a Package>,
        ) -> Result<()> {
            if done.contains(id) {
                return Ok(());
            }
            ensure!(active.insert(id.into()), "Guest package dependency cycle");
            let package = catalog
                .packages
                .iter()
                .find(|p| p.id == id)
                .context("Unknown guest package")?;
            for dependency in &package.requires {
                if let Some(version) = package.required_versions.get(dependency) {
                    ensure!(
                        catalog
                            .packages
                            .iter()
                            .any(|p| &p.id == dependency && &p.version == version),
                        "Guest package dependency version mismatch"
                    );
                }
                visit(dependency, catalog, active, done, result)?;
            }
            active.remove(id);
            done.insert(id.into());
            result.push(package);
            Ok(())
        }
        let (mut active, mut done, mut result) = (BTreeSet::new(), BTreeSet::new(), Vec::new());
        for id in selected {
            visit(id, self, &mut active, &mut done, &mut result)?;
        }
        let target = (&result[0].os, &result[0].arch);
        ensure!(
            result.iter().all(|p| (&p.os, &p.arch) == target),
            "Packages for different guest targets cannot be combined"
        );
        let mut destinations = BTreeMap::new();
        ensure!(
            result.iter().map(|p| p.files.len()).sum::<usize>() <= MAX_TOTAL_FILES,
            "Guest package plan has too many payloads"
        );
        for p in &result {
            for f in &p.files {
                if !f.destination.is_empty() {
                    let key = f.destination.to_ascii_lowercase();
                    ensure!(
                        destinations.insert(key, &f.sha256).is_none(),
                        "Conflicting package destinations"
                    );
                }
            }
        }
        Ok(result)
    }
}

fn verified_copy(root: &Path, source: &str, expected: &str, destination: &Path) -> Result<()> {
    ensure!(relative(source), "Unsafe package source path");
    let mut path = root.to_path_buf();
    for component in Path::new(source).components() {
        path.push(component);
        ensure!(
            !fs::symlink_metadata(&path)?.file_type().is_symlink(),
            "Package sources must not traverse symbolic links"
        );
    }
    let file = fs::symlink_metadata(&path)?;
    ensure!(
        file.file_type().is_file() && file.len() <= MAX_PAYLOAD_BYTES,
        "Invalid or oversized package payload"
    );
    fs::create_dir_all(destination.parent().context("No payload parent")?)?;
    fs::copy(&path, destination)?;
    let mut input = fs::File::open(destination)?;
    let mut sha = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let bytes = input.read(&mut buffer)?;
        if bytes == 0 {
            break;
        }
        sha.update(&buffer[..bytes]);
    }
    ensure!(
        format!("{:x}", sha.finalize()) == expected,
        "Package payload hash mismatch: {source}"
    );
    Ok(())
}

fn snapshot(
    root: &Path,
    selected: &[String],
    setup: &Path,
    output: &Path,
) -> Result<tempfile::TempDir> {
    let catalog = Catalog::load(root)?;
    let packages = catalog.resolve(selected)?;
    ensure!(
        setup.is_file() && !fs::symlink_metadata(setup)?.file_type().is_symlink(),
        "Build the guest package setup backend first"
    );
    fs::create_dir_all(output.parent().context("No media parent")?)?;
    let staging = tempfile::tempdir_in(output.parent().unwrap())?;
    fs::copy(setup, staging.path().join(SETUP_NAME))?;
    let mut plan = format!(
        "[setup]\nschema={SCHEMA}\nos={}\narch={}\npackages={}\n",
        packages[0].os,
        packages[0].arch,
        packages.len()
    );
    let mut payload_bytes = 0u64;
    for (index, p) in packages.iter().enumerate() {
        for f in std::iter::once(&p.license).chain(p.files.iter()) {
            payload_bytes = payload_bytes
                .checked_add(fs::metadata(root.join(&f.source))?.len())
                .context("Package size overflow")?;
        }
        ensure!(
            payload_bytes <= MAX_TOTAL_PAYLOAD_BYTES,
            "Guest package media exceeds its size limit"
        );
        plan.push_str(&format!(
            "\n[package{index}]\nid={}\nversion={}\nfiles={}\ndrivers={}\n",
            p.id,
            p.version,
            p.files.len(),
            p.drivers.len()
        ));
        verified_copy(
            root,
            &p.license.source,
            &p.license.sha256,
            &staging.path().join(&p.id).join("LICENSE.txt"),
        )?;
        for (file_index, f) in p.files.iter().enumerate() {
            verified_copy(
                root,
                &f.source,
                &f.sha256,
                &staging.path().join(&p.id).join(&f.source),
            )?;
            let destination = f.destination.replace('/', "\\");
            plan.push_str(&format!("\n[file{index}_{file_index}]\nsource={}\\{}\nsha256={}\ndestination={}\nprevious={}\nreboot={}\nboot_service={}\n",p.id,f.source.replace('/',"\\"),f.sha256,destination,f.previous_sha256.join(","),u8::from(f.reboot),f.boot_service));
        }
        for (driver_index, d) in p.drivers.iter().enumerate() {
            plan.push_str(&format!(
                "\n[driver{index}_{driver_index}]\ninf={}\\{}\nhardware_ids={}\n",
                p.id,
                d.inf.replace('/', "\\"),
                d.hardware_ids.join("|")
            ));
        }
    }
    fs::write(staging.path().join("setup.ini"), plan)?;
    fs::write(
        staging.path().join("autorun.inf"),
        format!("[autorun]\nopen={SETUP_NAME}\nlabel=Qemu Guest Packages\n"),
    )?;
    Ok(staging)
}

/// Produce an immutable snapshot. Payloads and licenses are checked after copying.
/// Guest destinations are relative to SystemRoot, never host paths or shell text.
pub fn prepare(root: &Path, selected: &[String], setup: &Path, output: &Path) -> Result<()> {
    let staging = snapshot(root, selected, setup, output)?;
    let temporary = tempfile::NamedTempFile::new_in(output.parent().unwrap())?;
    let result = Command::new("xorriso")
        .args([
            "-as",
            "mkisofs",
            "-quiet",
            "-J",
            "-r",
            "-V",
            "QEMU_PACKAGES",
            "-o",
        ])
        .arg(temporary.path())
        .arg(staging.path())
        .output()
        .context("Guest package media requires xorriso")?;
    if !result.status.success() {
        bail!(
            "Guest package media failed: {}",
            String::from_utf8_lossy(&result.stderr)
        );
    }
    temporary.persist(output).map_err(|e| e.error)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn package(id: &str, requires: &[&str]) -> Package {
        let file = Payload {
            source: "payload.bin".into(),
            sha256: "a".repeat(64),
            destination: "system32/drivers/device.sys".into(),
            previous_sha256: vec![],
            reboot: false,
            boot_service: String::new(),
        };
        Package {
            id: id.into(),
            label: id.into(),
            version: "1".into(),
            os: "reactos".into(),
            arch: "i386".into(),
            requires: requires.iter().map(|s| s.to_string()).collect(),
            required_versions: BTreeMap::new(),
            license: Payload {
                destination: String::new(),
                ..file.clone()
            },
            files: vec![Payload {
                destination: format!("system32/{id}.bin"),
                ..file
            }],
            drivers: vec![],
        }
    }
    #[test]
    fn rejects_dependency_version_mismatch() {
        let mut driver = package("device", &["core"]);
        driver.required_versions.insert("core".into(), "2".into());
        let mut catalog = Catalog {
            schema: SCHEMA,
            packages: vec![package("core", &[]), driver],
        };
        assert!(catalog.resolve(&["device".into()]).is_err());
        catalog.packages[0].version = "2".into();
        assert!(catalog.resolve(&["device".into()]).is_ok());
    }
    #[test]
    fn resolves_transitive_dependencies_once() {
        let c = Catalog {
            schema: SCHEMA,
            packages: vec![
                package("network", &["core", "firmware"]),
                package("core", &[]),
                package("firmware", &["core"]),
            ],
        };
        assert_eq!(
            c.resolve(&["network".into()])
                .unwrap()
                .iter()
                .map(|p| p.id.as_str())
                .collect::<Vec<_>>(),
            vec!["core", "firmware", "network"]
        );
    }
    #[test]
    fn rejects_cycles_missing_and_duplicate_ids() {
        for packages in [
            vec![package("a", &["b"]), package("b", &["a"])],
            vec![package("a", &["missing"])],
            vec![package("a", &[]), package("a", &[])],
        ] {
            assert!(Catalog {
                schema: SCHEMA,
                packages
            }
            .resolve(&["a".into()])
            .is_err());
        }
    }
    #[test]
    fn rejects_cross_target_and_conflicting_payloads() {
        let mut a = package("a", &[]);
        let mut b = package("b", &[]);
        b.os = "windows".into();
        assert!(Catalog {
            schema: SCHEMA,
            packages: vec![a.clone(), b.clone()]
        }
        .resolve(&["a".into(), "b".into()])
        .is_err());
        b.os = "reactos".into();
        b.files[0].destination = a.files[0].destination.clone();
        a.files[0].sha256 = "b".repeat(64);
        assert!(Catalog {
            schema: SCHEMA,
            packages: vec![a, b]
        }
        .resolve(&["a".into(), "b".into()])
        .is_err());
    }
    #[test]
    fn rejects_paths_and_unreviewed_replacements() {
        for path in [
            "../a",
            "/root",
            "C:/a",
            "x/../a",
            "x\\a",
            "a/",
            "a.",
            "x\nkey=bad",
            "CON.txt",
            "x/LPT1.bin",
            "x?y",
            "x\u{0001}y",
        ] {
            assert!(!relative(path), "{path}");
        }
        let mut p = package("a", &[]);
        p.files[0].reboot = true;
        assert!(Catalog {
            schema: SCHEMA,
            packages: vec![p]
        }
        .validate()
        .is_err());
    }
    #[test]
    fn validates_copied_bytes_and_rejects_symlinks() {
        let root = tempfile::tempdir().unwrap();
        let out = tempfile::tempdir().unwrap();
        fs::write(root.path().join("data"), b"checked payload").unwrap();
        let expected = format!("{:x}", Sha256::digest(b"checked payload"));
        assert!(verified_copy(root.path(), "data", &expected, &out.path().join("copy")).is_ok());
        assert!(verified_copy(
            root.path(),
            "data",
            &"0".repeat(64),
            &out.path().join("bad")
        )
        .is_err());
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(root.path().join("data"), root.path().join("link")).unwrap();
            assert!(
                verified_copy(root.path(), "link", &expected, &out.path().join("link")).is_err()
            );
        }
    }
}
