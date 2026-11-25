"""Guards for interface fixes that a merge from upstream could quietly undo."""
import html
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def translations(context):
    ts = read("res/translations/ru_RU.ts")
    block = re.search(r"<context>\s*<name>%s</name>(.*?)</context>" % re.escape(context), ts, re.S)
    found = {}
    if block:
        for m in re.finditer(r"<source>(.*?)</source>\s*<translation([^>]*)>(.*?)</translation>", block.group(1), re.S):
            done = "unfinished" not in m.group(2) and m.group(3).strip()
            found[html.unescape(m.group(1))] = bool(done)
    return found


def tr_strings(path):
    text = read(path)
    out = {"n": "\n", "t": "\t", '"': '"', "\\": "\\"}
    result = set()
    for m in re.finditer(r'(?<![\w:])tr\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)[,)]', text):
        parts = re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(1))
        result.add("".join(re.sub(r"\\(.)", lambda e: out.get(e.group(1), e.group(0)), p) for p in parts))
    return result


class InterfaceRegressions(unittest.TestCase):
    def test_advanced_interface_is_the_default(self):
        self.assertNotRegex(read("src/database/SettingsRepo.cpp"), r"simple_mode\s*=\s*true")

    def test_retired_dpi_tuning_does_not_remove_address_scanning(self):
        cmake = read("CMakeLists.txt")
        self.assertNotIn("TuneWorker", cmake)
        self.assertNotIn("menu_find_settings", read("include/ui/mainwindow.ui"))
        for text in ("Generic scan", "WARP scan", "DNS resolver scan"):
            self.assertIn(text, read("src/ui/scanner/dialog_new_scan.cpp"))
        self.assertIn("Resolver = 2, Retired = 3", read("include/database/entities/IpScan.h"))
        self.assertIn("Kind::Retired", read("src/ui/scanner/dialog_scan_instance.cpp"))
        self.assertNotIn("DROP COLUMN", read("src/database/IpScansRepo.cpp"))

    def test_process_lookup_is_forced_only_on_windows(self):
        text = read("src/configs/generate.cpp")
        at = text.index('route["find_process"] = true')
        guard = text.rfind("#ifdef Q_OS_WIN", 0, at)
        self.assertNotEqual(guard, -1)
        self.assertNotIn("#endif", text[guard:at])

    def test_frame_waits_only_for_dialog_windows(self):
        body = read("src/ui/widget/NewHome.cpp").split("void MainWindow::syncWindowFrame()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("dialog->isWindow()", body)

    def test_pastel_selection_matches_the_phone(self):
        # Match the phone's light and dark Pastel selection tones.
        text = read("src/ui/widget/NewHome.cpp")
        self.assertIn("tint == text() ? accent() : tint", text)
        self.assertIn("p.setPen(QPen(withAlpha(mark, day ? 200 : 230), 1.5));", text)
        self.assertIn("p.setBrush(blend(plate, mark, day ? .12 : .22));", text)

    def test_windows_and_menus_have_no_black_corners(self):
        text = read("src/ui/widget/NewHome.cpp")
        self.assertIn("clipRoundedWindow(widget, menuRadius);", text)
        self.assertIn("if (auto *menu = qobject_cast<QMenu *>(widget)) menu->clearMask();", text)
        # A clear fill on a child of the sheet cuts a hole down to the desktop.
        picker = text.split("void NewHome::showGroupPicker()", 1)[1].split("\n}\n", 1)[0]
        self.assertNotIn("setAttribute(Qt::WA_TranslucentBackground)", picker)
        self.assertIn("list->setAutoFillBackground(false);", picker)
        # Every dialog's frame follows the theme.
        self.assertIn("NewStyle::themeFrame(widget);", text)

    def test_tray_menu_is_see_through_from_birth(self):
        setup = read("src/ui/mainWindow/mainwindow_setup.cpp")
        made = setup.index("trayMenu = new QMenu();")
        self.assertLess(setup.index("trayMenu->setAttribute(Qt::WA_TranslucentBackground);"), setup.index("tray->setContextMenu(trayMenu);"))
        self.assertGreater(setup.index("trayMenu->setAttribute(Qt::WA_TranslucentBackground);"), made)

    def test_windows_dialog_switches_use_the_same_native_handle(self):
        text = read("src/ui/widget/NewHome.cpp")
        frame = text.split("void frameDialog(QDialog *dialog)", 1)[1].split("Qt::Edges dialogEdges", 1)[0]
        self.assertIn("#ifndef Q_OS_WIN\n        dialog->setProperty", frame)
        self.assertIn("#ifdef Q_OS_WIN\n            recalculateWindowsFrame(dialog);", frame)
        self.assertIn('window->property("newFrame").toBool() && handleWindowsFrame', text)

    def test_centered_panel_takes_the_whole_column(self):
        text = read("src/ui/widget/NewHome.cpp")
        block = text.split("void NewHome::applyArrangement()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("m_side->setMaximumWidth(QWIDGETSIZE_MAX);", block)

    def test_tabs_keep_their_titles_at_the_minimum_width(self):
        tile = read("src/ui/widget/NewHome.cpp").split("QSize NewTile::minimumSizeHint()", 1)[1].split("\n}", 1)[0]
        self.assertIn("m_kind == Text ? QSize(40,", tile)
        # The minimum is worked out for the new arrangement and leaves out the centering room,
        # which follows the window's width and would otherwise keep it from narrowing.
        self.assertIn("setMinimumSize(newHome->windowMinimum()", read("src/ui/mainWindow/mainwindow_view.cpp"))
        body = read("src/ui/widget/NewHome.cpp").split("QSize NewHome::windowMinimum()", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("layout()->activate();", body)
        self.assertIn("size.rwidth() -= room.left() + room.right();", body)

    def test_windows_tray_menu_is_shown_by_rawline(self):
        setup = read("src/ui/mainWindow/mainwindow_setup.cpp")
        win = setup.split("// Shown by Rawline itself", 1)[1].split("#endif", 1)[0]
        self.assertIn("trayMenu->popup(QCursor::pos());", win)
        self.assertIn("tray->setContextMenu(trayMenu);", win.split("#else", 1)[1])

    def test_tab_close_button_is_drawn_by_the_new_style(self):
        self.assertIn("case PE_IndicatorTabClose:", read("src/ui/widget/NewHome.cpp"))

    def test_arranging_lives_in_the_theme_settings(self):
        self.assertNotIn('addAction(T("Arrange the window"))', read("src/ui/widget/NewHome.cpp"))
        self.assertIn('"Arrange the window"', read("src/ui/setting/dialog_basic_settings.cpp"))

    def test_dns_rules_reach_the_config_and_are_self_tested(self):
        gen = read("src/configs/generate.cpp")
        self.assertIn("DecodeDnsRules(dataManager->settingsRepo->dns_rules)", gen)
        self.assertIn('hopTag("dns-user", userIndex++)', gen)
        self.assertIn('{"dns_rules",                  &dns_rules}', read("src/database/SettingsRepo.cpp"))
        self.assertIn("--self-test-dns-rules", read(".github/workflows/build.yml"))
        self.assertIn("settingsRepo->dns_rules = dns_rules_text;", read("src/ui/setting/dialog_manage_routes.cpp"))

    def test_dns_rules_speak_every_shipped_language(self):
        sources = tr_strings("src/ui/setting/dialog_manage_routes.cpp")
        for name in ["DNS Rules", "Delete the rule?", "Directly, without the proxy", "%1 and %2 more"]:
            self.assertIn(name, sources)
        for ts in ["ru_RU", "zh_CN", "fa_IR"]:
            text = read("res/translations/%s.ts" % ts)
            for source in ["DNS Rules", "Delete the rule?", "Directly, without the proxy", "Unknown kind «%1:» in «%2»"]:
                with self.subTest(ts=ts, source=source):
                    self.assertIn("<source>%s</source>" % source, text)

    def test_protocol_deploys_live_beside_the_panel(self):
        relay = read("src/ui/setting/dialog_vk_relay.cpp")
        self.assertIn('"Деплой WDTT Panel"', relay)
        self.assertIn('"Деплой " + kindTitle(kind)', relay)
        protocol = read("core/internal/relay/protocol.go")
        self.assertIn('case "deploy", "uninstall":', protocol)
        self.assertIn('case "panel":', protocol)

    def test_new_screens_are_translated_to_russian(self):
        # Names of protocols and the like stay as they are.
        keep = {"example.com", "AdBlock", "TCP Brutal", "UDP/443"}
        for contexts, path in [
            (["RouteDnsEditor"], "src/ui/setting/RouteDnsEditor.cpp"),
            (["ProcessPicker"], "src/ui/setting/ProcessPicker.cpp"),
            (["DialogRuleTester"], "src/ui/setting/dialog_rule_tester.cpp"),
            (["MuxSettingsWidget", "XrayMuxSettingsWidget"], "src/ui/widget/MuxSettingsWidget.cpp"),
        ]:
            done = {}
            for context in contexts:
                for source, ok in translations(context).items():
                    done[source] = done.get(source) or ok
            for source in tr_strings(path):
                if not re.search(r"[A-Za-z]{3}", source) or source in keep:
                    continue
                with self.subTest(path=path, source=source):
                    self.assertTrue(done.get(source), "no Russian translation")


if __name__ == "__main__":
    unittest.main()
