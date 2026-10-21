"""Synthetic tests: grammar extraction must not confuse prose, examples or return codes with CLI options."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("grammar", Path(__file__).resolve().parents[1] / "update-parameter-grammar.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class GrammarTests(unittest.TestCase):
    def test_explicit_prose_constraints_override_optional_heading(self):
        for noun in ("параметра", "ключа"):
            with self.subTest(noun=noun):
                required, _ = module.grammar_rules(["/ManageCfgSupport [-disableSupport [-force]]",
                    f"● -disableSupport — операция. При отсутствии {noun} генерируется ошибка."])
                self.assertIn(((), ("-disableSupport",)), required)
        required, _ = module.grammar_rules(["/DeleteCfg [-Extension <name>] [-AllExtensions]",
            "Использование команды без параметра не допускается."])
        self.assertIn(((), ("-AllExtensions", "-Extension")), required)

    def test_explicit_mode_exclusions_are_version_evidence(self):
        syntax = "/CreateDistributive <dir> -File <file> [-MakeSetup] [-MakeFiles]"
        _, conflicts = module.grammar_rules([syntax, "Из параметров -MakeSetup и -MakeFiles может быть использован только один."])
        self.assertIn(("-MakeFiles", "-MakeSetup"), conflicts)
        self.assertNotIn(("-MakeFiles", "-MakeSetup"), module.grammar_rules([syntax])[1])

    def test_quoted_values_are_distinct_from_other_option_values(self):
        parts = ["/Example [-files «<files>»] [-Count <n>] [-comment <text>]",
                 "● -comment <text> — комментарий должен быть в двойных кавычках."]
        self.assertEqual(module.quoted_operands(parts), ["-comment", "-files"])

    def test_cross_command_rule_does_not_confuse_local_switch(self):
        parts = ["/Example [-Z: <values>]",
                 "Команда /Example не может использоваться одновременно с командой /Z."]
        self.assertEqual(module.cross_command_conflicts(parts), [("/example", "/z")])
        self.assertEqual(module.cross_command_conflicts(["/Example [-Z: <values>]", "Используется локальный параметр -Z."]), [])

    def test_dash_prefixed_operand_values_are_not_independent_options(self):
        parts = ["/Example [<mode>] [-attach]", "Транспорт задаётся значением <mode>:",
                 "● -tcp — первый транспорт.", "● -http — второй транспорт.",
                 "Опция -attach применима ко второму транспорту."]
        self.assertEqual(module.primary_values(parts), ["-tcp", "-http"])
        self.assertEqual(module.options(parts), [("-attach", "-attach", "flag", False)])
        self.assertEqual(module.enum_values(parts), [("", "-tcp"), ("", "-http")])
        self.assertEqual(module.primary_operand_count(parts), 0)

    def test_applicability_conditions_are_not_literal_names(self):
        parts = ["/Example [-Name <name>]", "● -Name — имя в зависимости от значения другого параметра:",
                 "● Vendor — имя поставщика.", "● Extension — имя расширения."]
        self.assertEqual(module.enum_values(parts), [])

    def test_inline_literal_domain_is_distinct_from_descriptive_placeholder(self):
        parts = ["/Example <имя файла|путь> [-Format <txt|mxl>] [-Tree Plain|Hierarchical]"]
        self.assertEqual(module.primary_values(parts), [])
        self.assertEqual(module.enum_values(parts), [("-Format", "txt"), ("-Format", "mxl"),
                                                  ("-Tree", "Plain"), ("-Tree", "Hierarchical")])

    def test_file_contents_are_not_command_operand_values(self):
        parts = ["/Example <file>", "Содержимое файла описывает режим:",
                 "● open — открытый режим.", "● hidden — скрытый режим."]
        self.assertEqual(module.enum_values(parts), [])

    def test_primary_list_ends_before_option_and_its_own_domain(self):
        parts = ["/Example [<mode>] [-Rule <rule>]", "Варианты <mode>:",
                 "● -tcp — транспорт.", "● -Rule — правило:", "● First — значение правила."]
        # A named option documented in the header is not an operand alternative.
        self.assertEqual(module.primary_values(parts), ["-tcp"])
        self.assertEqual(module.enum_values(parts), [("", "-tcp"), ("-Rule", "First")])

    def test_multiple_options_and_values(self):
        parts = ["/Example [-NoBind] [-Count <n>] [-Rule <r>]",
                 "● -Rule <r> — допустимые значения:", "● First — первый вариант.", "● Second — второй вариант.",
                 "Код возврата:", "● 0 — успешно.", "● 1 — ошибка."]
        self.assertEqual(module.options(parts), [("-NoBind", "-NoBind", "flag", False),
            ("-Count", "-Count <значение>", "space", False), ("-Rule", "-Rule <значение>", "space", False)])
        self.assertEqual(module.enum_values(parts), [("-Rule", "First"), ("-Rule", "Second")])

    def test_separators_and_optional_operand(self):
        result = module.options(["/Example [-Call<n>] [-TURL=<url>] [-TimeLimit:hhh:mm] [-LogIntegrity [MDtype[,MDtype]]]"])
        self.assertEqual([x[2] for x in result], ["attached", "equals", "colon", "space"])
        self.assertTrue(result[-1][3])

    def test_definitions_do_not_mix_bare_commands_or_web(self):
        entries = module.definitions([("h2", "Общие команды"), ("p", "/Example [-Count <n>]"),
            ("p", "Описание."), ("p", "UsePrivilegedMode"), ("p", "Иное описание."),
            ("h2", "Параметры веб-клиента"), ("p", "/WebOnly [-WebFlag]")])
        self.assertEqual(entries["/example"], ["/Example [-Count <n>]", "Описание."])
        self.assertNotIn("/webonly", entries)
        self.assertEqual(entries["useprivilegedmode"], ["UsePrivilegedMode", "Иное описание."])

    def test_historical_heading_punctuation_is_not_a_different_command(self):
        blocks = [("h2", "Общие команды"), ("p", "/DisableUnrecoverableErrorMessage."),
                  ("p", "Описание."), ("p", "DisableHomePageForms"), ("p", "Описание.")]
        expected = {"/disableunrecoverableerrormessage", "/disablehomepageforms"}
        self.assertEqual(set(module.definitions(blocks)), expected)
        self.assertEqual(set(module.versions_module.commands(blocks)), expected)

    def test_inline_creation_options_have_optional_operands(self):
        self.assertEqual(module.syntax_only("/AddToList ‑ собственное пояснение."), "/AddToList [<имя базы>]")
        self.assertEqual(module.syntax_only("/AddInList ‑ собственное пояснение."), "/AddInList [<имя базы>]")
        self.assertEqual(module.primary_operand_count(["/UseTemplate ‑ собственное пояснение."]), 0)

    def test_historical_option_typo_uses_the_header_spelling(self):
        parts = ["/Example [-FirstVersion <number>]", "● -FitstVersion <number> ‑ номер версии."]
        self.assertEqual([o[0] for o in module.options(parts)], ["-FirstVersion"])

    def test_typographic_minus_can_be_an_enum_value(self):
        self.assertEqual(module.enum_values(["/Example [-Dynamic <mode>]", "● -Dynamic <mode> ‑ режим:",
            "● + ‑ включить.", "● – ‑ отключить."]), [("-Dynamic", "+"), ("-Dynamic", "-")])

    def test_body_defaults_override_two_known_incomplete_headers(self):
        parts = ["/CreateDistributivePackage <dir> -File <file> -PackageFileName <name>",
                 "Если опция не задана, используется updsetup.zip."]
        required, _ = module.grammar_rules(parts)
        self.assertIn(((), ("-File",)), required)
        self.assertNotIn(((), ("-PackageFileName",)), required)
        self.assertEqual(module.primary_operand_count(["/CreateTemplateListFile <name>",
            "Если не указано имя файла, используется имя по умолчанию."]), 0)

    def test_header_and_bullet_only_option_are_both_collected(self):
        result = module.options(["/Example [-One]", "● -Other <имя> — другой параметр."])
        self.assertEqual([x[0] for x in result], ["-One", "-Other"])
        self.assertEqual(result[1][2], "space")

    def test_inline_description_is_not_serialized(self):
        self.assertEqual(module.syntax_only("/Example <path> — синтетическое описание."), "/Example <path>")

    def test_flag_does_not_inherit_return_code_enumeration(self):
        self.assertEqual(module.enum_values(["/Example [-force]", "● -force — варианты выполнения:",
            "Коды завершения:", "● 0 — успех.", "● 1 — ошибка."]), [])

    def test_required_proxy_and_nested_credentials(self):
        required, conflicts = module.grammar_rules(["/Proxy -PSrv <address> -PPort <port> [-PUser <user> [-PPwd <password>]]"])
        self.assertIn(((), ("-PSrv",)), required)
        self.assertIn(((), ("-PPort",)), required)
        self.assertIn((("-PPwd", "-PUser"), ("-PUser",)), required)
        self.assertNotIn(((), ("-PUser",)), required)
        self.assertEqual(conflicts, [])

    def test_optional_sequence_requires_all_its_members(self):
        required, _ = module.grammar_rules(["/Example [-Allow -First <rule> -Second <rule>] [-Other]"])
        self.assertEqual(len(required), 3)
        self.assertTrue(all(triggers == ("-Allow", "-First", "-Second") for triggers, _ in required))

    def test_alternatives_and_operand_pipes(self):
        required, conflicts = module.grammar_rules(["/Example [-A | -B] [-Format Plain|Hierarchical] [-File <a|b>] [-Log [MDtype[,MDtype]]]"])
        self.assertEqual(required, [])
        self.assertEqual(conflicts, [("-A", "-B")])

    def test_required_alternative_and_repeated_group(self):
        required, conflicts = module.grammar_rules(["/Example [-Update <file> [-f <file>|-v <version>]+]"])
        self.assertIn((("-Update", "-f", "-v"), ("-f", "-v")), required)
        self.assertEqual(conflicts, [])

    def test_typographic_switches_keep_their_documented_order(self):
        parts = ["/Example –First <a> [-Optional] ‑Second <b>"]
        self.assertEqual([option[0] for option in module.options(parts)], ["-First", "-Optional", "-Second"])
        required, _ = module.grammar_rules(parts)
        self.assertEqual(required, [((), ("-First",)), ((), ("-Second",))])

    def test_quoted_value_without_angle_brackets(self):
        option = module.options(["/Example [-Z: «значения разделителей»]"])[0]
        self.assertEqual(option[2:], ("colon", False))
        self.assertEqual(module.options(["/Example -files “<files>”"])[0][2:], ("space", False))

    def test_load_files_header_is_reconciled_with_option_descriptions(self):
        required, _ = module.grammar_rules(["/LoadConfigFromFiles <dir> -files <list> -listFile <path> -Format <mode> -partial"])
        self.assertEqual(required, [])

    def test_partial_load_requires_explicit_format_only_if_documented(self):
        intro = "Для частичной загрузки автоматического определения формата нет: укажите -Format."
        required, _ = module.grammar_rules(["/LoadConfigFromFiles <dir> -files <list> -listFile <path> -Format <mode>", intro])
        self.assertIn((("-files", "-listFile"), ("-Format",)), required)
        required, _ = module.grammar_rules(["/LoadConfigFromFiles <dir> -files <list> -listFile <path>", intro])
        self.assertEqual(required, [])

    def test_bad_brackets_fail_refresh(self):
        with self.assertRaises(ValueError):
            module.grammar_rules(["/Example [-A [-B]"])

    def test_positional_operands_are_separate_from_option_values(self):
        self.assertEqual(module.primary_operand_count(["/Example <dir> <file> [-X <x>] -Settings <settings>"]), 2)
        self.assertEqual(module.primary_operand_count(["/Proxy -PSrv <addr> -PPort <port>"]), 0)
        self.assertEqual(module.primary_operand_count(["/Example [<mode>] [-Call<n>]"]), 0)
        self.assertEqual(module.primary_operand_count(["/Example [-v <version>] [-name] <label> [-comment <text>]"]), 1)


if __name__ == "__main__":
    unittest.main()
