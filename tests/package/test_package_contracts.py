import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class DebianPackageContractTest(unittest.TestCase):
    def read(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_binary_packages_and_passive_systemd_policy_are_declared(self):
        control = self.read("debian/control")
        for package in ("infiniswap-dkms", "infiniswap-provider", "infiniswapctl"):
            self.assertIn("Package: " + package, control)

        service = self.read("debian/infiniswap-provider.service")
        self.assertIn(
            "ConditionPathExists=/etc/infiniswap/provider-memory.conf", service
        )
        self.assertIn("ConditionPathExists=/etc/infiniswap/consumers.conf", service)
        self.assertIn("LimitMEMLOCK=infinity", service)
        self.assertNotRegex(service, r"\b(?:swapon|swapoff|mkswap)\b")

        rules = self.read("debian/rules")
        self.assertIn("dh_dkms -V $(PACKAGE_VERSION)", rules)
        self.assertIn("dh_installsystemd --no-enable --no-start", rules)

    def test_dkms_contract_is_versioned_bounded_and_has_optional_signing(self):
        dkms = self.read("debian/infiniswap-dkms.dkms")
        self.assertIn('PACKAGE_VERSION="#MODULE_VERSION#"', dkms)
        self.assertIn('AUTOINSTALL="yes"', dkms)
        self.assertIn("dkms-preflight.sh", dkms)
        self.assertIn("dkms-sign-module.sh", dkms)

        preflight = self.read("infiniswap_bd/scripts/dkms-preflight.sh")
        self.assertIn("5.15.*|6.8.*", preflight)
        self.assertIn("install the exact linux-headers", preflight)

        signing = self.read("infiniswap_bd/scripts/dkms-sign-module.sh")
        self.assertIn("INFINISWAP_SIGN_KEY", signing)
        self.assertIn("scripts/sign-file", signing)
        self.assertNotRegex(signing, r"BEGIN (?:RSA |EC )?PRIVATE KEY")
        self.assertNotRegex(
            self.read("debian/module-signing.conf"), r"BEGIN .*PRIVATE KEY"
        )

    def test_package_sources_contain_no_implicit_swap_mutation(self):
        forbidden = re.compile(r"\b(?:swapon|mkswap)\b|swapoff\s+-a")
        for path in (ROOT / "debian").rglob("*"):
            if path.is_file():
                try:
                    content = path.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    continue
                self.assertIsNone(forbidden.search(content), path)

    def test_package_manifests_include_metrics_schemas_defaults_and_docs(self):
        provider = self.read("debian/infiniswap-provider.install")
        ctl = self.read("debian/infiniswapctl.install")
        self.assertIn("infiniswap-alerts.yml", provider)
        self.assertIn("provider-memory.example.conf", provider)
        self.assertIn("consumers.example.conf", provider)
        self.assertIn("usr/share/infiniswap/*.json", ctl)
        self.assertTrue((ROOT / "docs/packaging.md").is_file())
        self.assertTrue((ROOT / "tests/package/run").is_file())


if __name__ == "__main__":
    unittest.main()
