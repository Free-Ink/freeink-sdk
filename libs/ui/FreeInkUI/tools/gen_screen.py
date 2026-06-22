#!/usr/bin/env python3
"""Generate a FreeInkApp screen function from a small JSON screen schema."""

import argparse
import json
import re
from pathlib import Path


def pascal(name: str) -> str:
    parts = re.split(r"[^A-Za-z0-9]+", name or "")
    out = "".join(p[:1].upper() + p[1:] for p in parts if p)
    return out or "Unnamed"


def ident(name: str, suffix: str = "") -> str:
    value = pascal(name) + suffix
    if value[0].isdigit():
        value = "_" + value
    return value


def cstr(value):
    if value is None:
        return "nullptr"
    return json.dumps(str(value))


def bool_literal(value) -> str:
    return "true" if value else "false"


def collect_actions(node, actions):
    if isinstance(node, dict):
        for action_key in (
            "action",
            "decrement",
            "increment",
            "shiftAction",
            "modeAction",
            "deleteAction",
            "okAction",
        ):
            action = node.get(action_key)
            if isinstance(action, str) and action and action not in actions:
                actions[action] = len(actions) + 1
        for key in ("children", "items", "buttons", "footer"):
            collect_actions(node.get(key), actions)
    elif isinstance(node, list):
        for item in node:
            collect_actions(item, actions)


def action_expr(action, actions):
    if action is None:
        return "freeink::ui::NO_ACTION"
    if isinstance(action, int):
        return str(action)
    return ident(action, "Action") if action in actions else "freeink::ui::NO_ACTION"


def emit_header(child, lines):
    args = [cstr(child.get("title")), cstr(child.get("subtitle")), cstr(child.get("rightLabel"))]
    while args and args[-1] == "nullptr":
        args.pop()
    lines.append(f"  screen.header({', '.join(args)});")


def emit_spacer(child, lines):
    lines.append(f"  screen.spacer({int(child.get('height', 0))});")


def emit_button(child, lines, actions):
    label = cstr(child.get("label"))
    action = action_expr(child.get("action"), actions)
    value = int(child.get("value", 0))
    if value:
        lines.append(f"  screen.button({label}, {action}, {value});")
    else:
        lines.append(f"  screen.button({label}, {action});")


def emit_footer(child, lines, actions, index):
    buttons = child.get("buttons", [])
    name = f"footerActions{index}"
    lines.append(f"  const freeink::ui::FooterAction {name}[] = {{")
    for button in buttons:
        label = cstr(button.get("label"))
        action = action_expr(button.get("action"), actions)
        value = int(button.get("value", 0))
        enabled = bool_literal(button.get("enabled", True))
        lines.append(f"      {{{label}, {action}, {value}, freeink::ui::StateNormal, {enabled}}},")
    lines.append("  };")
    lines.append(f"  screen.footer({name}, {len(buttons)});")


def emit_list(child, lines, actions, index):
    items = child.get("items", [])
    name = f"listItems{index}"
    lines.append(f"  static const freeink::ui::ListItem {name}[] = {{")
    for item in items:
        label = cstr(item.get("label"))
        subtitle = cstr(item.get("subtitle"))
        value = cstr(item.get("value"))
        action_value = int(item.get("actionValue", item.get("valueId", 0)))
        enabled = bool_literal(item.get("enabled", True))
        is_header = bool_literal(item.get("isHeader", False))
        lines.append(
            f"      {{{label}, {subtitle}, {value}, {{}}, {{}}, freeink::ui::StateNormal, "
            f"{action_value}, {enabled}, {is_header}}},"
        )
    lines.append("  };")
    action = action_expr(child.get("action"), actions)
    selected = int(child.get("selectedIndex", -1))
    top = int(child.get("topIndex", 0))
    lines.append(f"  screen.list({name}, {len(items)}, {selected}, {action}, {top});")


def emit_setting_row(child, lines, actions, index):
    name = f"settingRow{index}"
    lines.append(f"  freeink::ui::SettingRowProps {name};")
    lines.append(f"  {name}.label = {cstr(child.get('label'))};")
    if child.get("subtitle") is not None:
        lines.append(f"  {name}.subtitle = {cstr(child.get('subtitle'))};")
    if child.get("value") is not None:
        lines.append(f"  {name}.value = {cstr(child.get('value'))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.action = {action_expr(child.get('action'), actions)};")
    if child.get("actionValue") is not None:
        lines.append(f"  {name}.valueId = {int(child.get('actionValue'))};")
    if child.get("chevron"):
        lines.append(f"  {name}.drawChevron = true;")
    lines.append(f"  screen.settingRow({name});")


def emit_toggle_row(child, lines, actions, index):
    name = f"toggleRow{index}"
    lines.append(f"  freeink::ui::ToggleRowProps {name};")
    lines.append(f"  {name}.row.label = {cstr(child.get('label'))};")
    if child.get("subtitle") is not None:
        lines.append(f"  {name}.row.subtitle = {cstr(child.get('subtitle'))};")
    lines.append(f"  {name}.checked = {bool_literal(child.get('checked', False))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.toggleAction = {action_expr(child.get('action'), actions)};")
    if child.get("value") is not None:
        lines.append(f"  {name}.toggleValue = {int(child.get('value'))};")
    if child.get("radius") is not None:
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
    if child.get("knobRadius") is not None:
        lines.append(f"  {name}.knobRadius = {int(child.get('knobRadius'))};")
    lines.append(f"  screen.toggleRow({name});")


def emit_stepper_row(child, lines, actions, index):
    name = f"stepperRow{index}"
    lines.append(f"  freeink::ui::StepperRowProps {name};")
    lines.append(f"  {name}.row.label = {cstr(child.get('label'))};")
    lines.append(f"  {name}.value = {cstr(child.get('value'))};")
    if child.get("decrement") is not None:
        lines.append(f"  {name}.decrement = {action_expr(child.get('decrement'), actions)};")
    if child.get("increment") is not None:
        lines.append(f"  {name}.increment = {action_expr(child.get('increment'), actions)};")
    if child.get("controlSize") is not None:
        lines.append(f"  {name}.controlSize = {int(child.get('controlSize'))};")
    if child.get("controlStroke") is not None:
        lines.append(f"  {name}.controlStroke = {int(child.get('controlStroke'))};")
    lines.append(f"  screen.stepperRow({name});")


def emit_radio_group(child, lines, actions, index):
    options = child.get("options", [])
    name = f"radioOptions{index}"
    lines.append(f"  static const freeink::ui::RadioOption {name}[] = {{")
    for option in options:
        lines.append(
            f"      {{{cstr(option.get('label'))}, {int(option.get('value', 0))}, "
            f"{bool_literal(option.get('enabled', True))}}},"
        )
    lines.append("  };")
    props = f"radioGroup{index}"
    lines.append(f"  freeink::ui::RadioGroupProps {props};")
    lines.append(f"  {props}.options = {name};")
    lines.append(f"  {props}.count = {len(options)};")
    lines.append(f"  {props}.selectedValue = {int(child.get('selectedValue', 0))};")
    if child.get("action") is not None:
        lines.append(f"  {props}.action = {action_expr(child.get('action'), actions)};")
    lines.append(f"  screen.radioGroup({props});")


def emit_popup(child, lines):
    lines.append(f"  screen.popup({cstr(child.get('message'))});")


def emit_qwerty_keyboard(child, lines, actions, index):
    name = f"qwertyKeyboard{index}"
    lines.append(f"  freeink::ui::QwertyKeyboardProps {name};")
    if child.get("action") is not None:
        lines.append(f"  {name}.keyAction = {action_expr(child.get('action'), actions)};")
    if child.get("shiftAction") is not None:
        lines.append(f"  {name}.shiftAction = {action_expr(child.get('shiftAction'), actions)};")
    if child.get("modeAction") is not None:
        lines.append(f"  {name}.modeAction = {action_expr(child.get('modeAction'), actions)};")
    if child.get("deleteAction") is not None:
        lines.append(f"  {name}.deleteAction = {action_expr(child.get('deleteAction'), actions)};")
    if child.get("okAction") is not None:
        lines.append(f"  {name}.okAction = {action_expr(child.get('okAction'), actions)};")
    if child.get("selectedIndex") is not None:
        lines.append(f"  {name}.selectedIndex = {int(child.get('selectedIndex'))};")
    if child.get("shifted") is not None:
        lines.append(f"  {name}.shifted = {bool_literal(child.get('shifted'))};")
    if child.get("symbols") is not None:
        lines.append(f"  {name}.symbols = {bool_literal(child.get('symbols'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.qwertyKeyboard({name}, {int(height)});")
    else:
        lines.append(f"  screen.qwertyKeyboard({name});")


def generate(schema):
    actions = {}
    collect_actions(schema, actions)
    screen_id = schema.get("screen") or schema.get("id") or "generated"
    fn = ident(screen_id, "Screen")
    max_interactions = int(schema.get("maxInteractions", 32))

    lines = [
        "#pragma once",
        "",
        "#include <FreeInkApp.h>",
        "",
    ]
    if actions:
        lines.append("enum : freeink::ui::ActionId {")
        for name, value in actions.items():
            lines.append(f"  {ident(name, 'Action')} = {value},")
        lines.append("};")
        lines.append("")
    lines.append(f"inline void {fn}(freeink::ui::Screen<{max_interactions}>& screen, void*) {{")

    children = schema.get("children", [])
    for index, child in enumerate(children):
        kind = child.get("type")
        if kind == "header":
            emit_header(child, lines)
        elif kind == "footer":
            emit_footer(child, lines, actions, index)
        elif kind == "list":
            emit_list(child, lines, actions, index)
        elif kind == "button":
            emit_button(child, lines, actions)
        elif kind == "settingRow":
            emit_setting_row(child, lines, actions, index)
        elif kind == "toggleRow":
            emit_toggle_row(child, lines, actions, index)
        elif kind == "stepperRow":
            emit_stepper_row(child, lines, actions, index)
        elif kind == "radioGroup":
            emit_radio_group(child, lines, actions, index)
        elif kind == "spacer":
            emit_spacer(child, lines)
        elif kind == "popup":
            emit_popup(child, lines)
        elif kind == "qwertyKeyboard":
            emit_qwerty_keyboard(child, lines, actions, index)
        else:
            raise SystemExit(f"unsupported component type: {kind!r}")
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("schema", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    schema = json.loads(args.schema.read_text(encoding="utf-8"))
    args.out.write_text(generate(schema), encoding="utf-8")


if __name__ == "__main__":
    main()
