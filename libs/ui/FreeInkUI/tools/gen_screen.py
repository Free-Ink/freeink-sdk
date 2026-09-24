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
        for key in ("children", "items", "buttons", "footer", "options"):
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


def edge_mask_expr(value):
    if value is None:
        return None
    if isinstance(value, int):
        return str(value)
    key = re.sub(r"[^a-z0-9]+", "", str(value).lower())
    values = {
        "none": "freeink::ui::EdgesNone",
        "top": "freeink::ui::EdgeTop",
        "right": "freeink::ui::EdgeRight",
        "bottom": "freeink::ui::EdgeBottom",
        "left": "freeink::ui::EdgeLeft",
        "horizontal": "freeink::ui::EdgesHorizontal",
        "topbottom": "freeink::ui::EdgesHorizontal",
        "vertical": "freeink::ui::EdgesVertical",
        "leftright": "freeink::ui::EdgesVertical",
        "all": "freeink::ui::EdgesAll",
    }
    return values.get(key, "freeink::ui::EdgesAll")


def text_align_expr(value):
    key = re.sub(r"[^a-z0-9]+", "", str(value or "").lower())
    if key == "center":
        return "freeink::ui::TextAlign::Center"
    if key == "right":
        return "freeink::ui::TextAlign::Right"
    return "freeink::ui::TextAlign::Left"


def anchor_expr(child, default="top"):
    anchor = str(child.get("anchor", default)).lower()
    if anchor == "bottom":
        return "freeink::ui::LayoutAnchor::Bottom"
    return "freeink::ui::LayoutAnchor::Top"


def anchor_arg(child, default="top"):
    if child.get("anchor") is None:
        return ""
    return f", {anchor_expr(child, default)}"


def insets_expr(value):
    if value is None:
        return None
    if isinstance(value, (int, float)):
        n = int(value)
        return f"freeink::ui::Insets{{{n}, {n}, {n}, {n}}}"
    if isinstance(value, list):
        values = [int(v) for v in value]
        if len(values) == 2:
            top, right = values
            return f"freeink::ui::Insets{{{top}, {right}, {top}, {right}}}"
        if len(values) == 4:
            top, right, bottom, left = values
            return f"freeink::ui::Insets{{{top}, {right}, {bottom}, {left}}}"
    if isinstance(value, dict):
        top = int(value.get("top", 0) or 0)
        right = int(value.get("right", 0) or 0)
        bottom = int(value.get("bottom", 0) or 0)
        left = int(value.get("left", 0) or 0)
        return f"freeink::ui::Insets{{{top}, {right}, {bottom}, {left}}}"
    return None


def emit_insets_assignment(lines, name, field, value):
    expr = insets_expr(value)
    if expr is not None:
        lines.append(f"  {name}.{field} = {expr};")


def keyboard_layout_expr(value):
    layouts = {
        "qwerty_en": "freeink::ui::KeyboardLayoutId::QwertyEn",
        "qwertyen": "freeink::ui::KeyboardLayoutId::QwertyEn",
        "en": "freeink::ui::KeyboardLayoutId::QwertyEn",
        "azerty_fr": "freeink::ui::KeyboardLayoutId::AzertyFr",
        "azertyfr": "freeink::ui::KeyboardLayoutId::AzertyFr",
        "fr": "freeink::ui::KeyboardLayoutId::AzertyFr",
        "qwertz_de": "freeink::ui::KeyboardLayoutId::QwertzDe",
        "qwertzde": "freeink::ui::KeyboardLayoutId::QwertzDe",
        "de": "freeink::ui::KeyboardLayoutId::QwertzDe",
        "spanish_es": "freeink::ui::KeyboardLayoutId::SpanishEs",
        "spanishes": "freeink::ui::KeyboardLayoutId::SpanishEs",
        "es": "freeink::ui::KeyboardLayoutId::SpanishEs",
    }
    key = re.sub(r"[^a-z0-9]+", "_", str(value or "qwerty_en").lower()).strip("_")
    return layouts.get(key, "freeink::ui::KeyboardLayoutId::QwertyEn")


def emit_header(child, lines):
    if child.get("borderEdges") is not None or child.get("centered") is not None:
        name = "headerProps"
        lines.append(f"  freeink::ui::HeaderProps {name};")
        if child.get("title") is not None:
            lines.append(f"  {name}.title = {cstr(child.get('title'))};")
        if child.get("subtitle") is not None:
            lines.append(f"  {name}.subtitle = {cstr(child.get('subtitle'))};")
        if child.get("rightLabel") is not None:
            lines.append(f"  {name}.rightLabel = {cstr(child.get('rightLabel'))};")
        if child.get("borderEdges") is not None:
            lines.append(f"  {name}.borderEdges = {edge_mask_expr(child.get('borderEdges'))};")
        if child.get("centered") is not None:
            lines.append(f"  {name}.centered = {bool_literal(child.get('centered'))};")
        lines.append(f"  screen.header({name}{anchor_arg(child)});")
        return
    args = [cstr(child.get("title")), cstr(child.get("subtitle")), cstr(child.get("rightLabel"))]
    while args and args[-1] == "nullptr":
        args.pop()
    if child.get("anchor") is not None:
        while len(args) < 3:
            args.append("nullptr")
        args.append(anchor_expr(child))
    lines.append(f"  screen.header({', '.join(args)});")


def emit_spacer(child, lines):
    lines.append(f"  screen.spacer({int(child.get('height', 0))}{anchor_arg(child)});")


def emit_button(child, lines, actions, index):
    label = cstr(child.get("label"))
    action = action_expr(child.get("action"), actions)
    value = int(child.get("value", 0))
    state = "freeink::ui::StateNormal"
    if child.get("radius") is not None:
        name = f"button{index}"
        lines.append(f"  freeink::ui::ButtonProps {name};")
        lines.append(f"  {name}.label = {label};")
        lines.append(f"  {name}.action = {action};")
        if value:
            lines.append(f"  {name}.value = {value};")
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
        lines.append(f"  screen.button({name}{anchor_arg(child)});")
    elif value or child.get("anchor") is not None:
        lines.append(f"  screen.button({label}, {action}, {value}, {state}{anchor_arg(child)});")
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
    if child.get("sidePadding") is not None or child.get("gap") is not None or child.get("buttonBorderEdges") is not None:
        props = f"footer{index}"
        lines.append(f"  freeink::ui::FooterProps {props};")
        lines.append(f"  {props}.actions = {name};")
        lines.append(f"  {props}.count = {len(buttons)};")
        if child.get("sidePadding") is not None:
            lines.append(f"  {props}.sidePadding = {int(child.get('sidePadding'))};")
        if child.get("gap") is not None:
            lines.append(f"  {props}.gap = {int(child.get('gap'))};")
        if child.get("buttonBorderEdges") is not None:
            lines.append(f"  {props}.buttonBorderEdges = {edge_mask_expr(child.get('buttonBorderEdges'))};")
        lines.append(f"  screen.footer({props}{anchor_arg(child, default='bottom')});")
    else:
        lines.append(f"  screen.footer({name}, {len(buttons)}{anchor_arg(child, default='bottom')});")


def selection_marker_expr(value):
    key = re.sub(r"[^a-z0-9]+", "", str(value or "").lower())
    if key == "underline":
        return "freeink::ui::SelectionMarker::Underline"
    if key == "triangle":
        return "freeink::ui::SelectionMarker::Triangle"
    return "freeink::ui::SelectionMarker::None"


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
    height = int(child.get("height", 0))
    if (
        child.get("rowRadius") is not None
        or child.get("sidePadding") is not None
        or child.get("rowGap") is not None
        or child.get("selectionMarker") is not None
    ):
        props = f"listProps{index}"
        lines.append(f"  freeink::ui::ListProps {props};")
        lines.append(f"  {props}.items = {name};")
        lines.append(f"  {props}.count = {len(items)};")
        lines.append(f"  {props}.selectedIndex = {selected};")
        lines.append(f"  {props}.topIndex = {top};")
        lines.append(f"  {props}.action = {action};")
        if child.get("rowRadius") is not None:
            lines.append(f"  {props}.rowRadius = {int(child.get('rowRadius'))};")
        if child.get("sidePadding") is not None:
            lines.append(f"  {props}.sidePadding = {int(child.get('sidePadding'))};")
        if child.get("rowGap") is not None:
            lines.append(f"  {props}.rowGap = {int(child.get('rowGap'))};")
        if child.get("selectionMarker") is not None:
            lines.append(f"  {props}.selectionMarker = {selection_marker_expr(child.get('selectionMarker'))};")
        if height or child.get("anchor") is not None:
            lines.append(f"  screen.list({props}, {height}, {anchor_expr(child)});")
        else:
            lines.append(f"  screen.list({props});")
        return
    if height or child.get("anchor") is not None:
        lines.append(f"  screen.list({name}, {len(items)}, {selected}, {action}, {top}, {height}, {anchor_expr(child)});")
    else:
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
    if child.get("radius") is not None:
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
    if child.get("sidePadding") is not None:
        lines.append(f"  {name}.sidePadding = {int(child.get('sidePadding'))};")
    if child.get("textGap") is not None:
        lines.append(f"  {name}.textGap = {int(child.get('textGap'))};")
    lines.append(f"  screen.settingRow({name}{anchor_arg(child)});")


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
    if child.get("rowRadius") is not None:
        lines.append(f"  {name}.row.radius = {int(child.get('rowRadius'))};")
    if child.get("sidePadding") is not None:
        lines.append(f"  {name}.row.sidePadding = {int(child.get('sidePadding'))};")
    if child.get("textGap") is not None:
        lines.append(f"  {name}.row.textGap = {int(child.get('textGap'))};")
    lines.append(f"  screen.toggleRow({name}{anchor_arg(child)});")


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
    if child.get("buttonRadius") is not None:
        lines.append(f"  {name}.buttonRadius = {int(child.get('buttonRadius'))};")
    if child.get("rowRadius") is not None:
        lines.append(f"  {name}.row.radius = {int(child.get('rowRadius'))};")
    if child.get("sidePadding") is not None:
        lines.append(f"  {name}.row.sidePadding = {int(child.get('sidePadding'))};")
    if child.get("textGap") is not None:
        lines.append(f"  {name}.row.textGap = {int(child.get('textGap'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    lines.append(f"  screen.stepperRow({name}{anchor_arg(child)});")


def emit_checkbox(child, lines, actions, index):
    name = f"checkbox{index}"
    lines.append(f"  freeink::ui::CheckboxProps {name};")
    lines.append(f"  {name}.label = {cstr(child.get('label'))};")
    lines.append(f"  {name}.checked = {bool_literal(child.get('checked', False))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.action = {action_expr(child.get('action'), actions)};")
    if child.get("value") is not None:
        lines.append(f"  {name}.value = {int(child.get('value'))};")
    if child.get("radius") is not None:
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
    if child.get("sidePadding") is not None:
        lines.append(f"  {name}.sidePadding = {int(child.get('sidePadding'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    lines.append(f"  screen.checkbox({name}{anchor_arg(child)});")


def emit_slider(child, lines, actions, index):
    name = f"slider{index}"
    lines.append(f"  freeink::ui::SliderProps {name};")
    lines.append(f"  {name}.value = {int(child.get('value', 0))};")
    lines.append(f"  {name}.max = {int(child.get('max', 100))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.action = {action_expr(child.get('action'), actions)};")
    if child.get("radius") is not None:
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.slider({name}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.slider({name}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.slider({name});")


def emit_dropdown(child, lines, actions, index):
    name = f"dropdown{index}"
    lines.append(f"  freeink::ui::DropdownProps {name};")
    if child.get("label") is not None:
        lines.append(f"  {name}.label = {cstr(child.get('label'))};")
    if child.get("value") is not None:
        lines.append(f"  {name}.value = {cstr(child.get('value'))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.action = {action_expr(child.get('action'), actions)};")
    if child.get("radius") is not None:
        lines.append(f"  {name}.radius = {int(child.get('radius'))};")
    if child.get("indicatorWidth") is not None:
        lines.append(f"  {name}.indicatorWidth = {int(child.get('indicatorWidth'))};")
    if child.get("indicatorSize") is not None:
        lines.append(f"  {name}.indicatorSize = {int(child.get('indicatorSize'))};")
    if child.get("indicatorStroke") is not None:
        lines.append(f"  {name}.indicatorStroke = {int(child.get('indicatorStroke'))};")
    emit_insets_assignment(lines, name, "padding", child.get("padding"))
    lines.append(f"  screen.dropdown({name}{anchor_arg(child)});")


def emit_table(child, lines, index):
    rows = child.get("rows", [["Name", "Value"], ["Rows", "2"]])
    row_count = len(rows)
    col_count = max((len(row) for row in rows), default=0)
    cells = []
    for row in rows:
        padded = list(row) + [""] * (col_count - len(row))
        cells.extend(padded)
    cells_name = f"tableCells{index}"
    lines.append(f"  static const char* const {cells_name}[] = {{")
    for cell in cells:
        lines.append(f"      {cstr(cell)},")
    lines.append("  };")
    name = f"table{index}"
    lines.append(f"  freeink::ui::TableProps {name};")
    lines.append(f"  {name}.cells = {cells_name};")
    lines.append(f"  {name}.rows = {row_count};")
    lines.append(f"  {name}.cols = {col_count};")
    if child.get("rowHeight") is not None:
        lines.append(f"  {name}.rowHeight = {int(child.get('rowHeight'))};")
    if child.get("headerRow") is not None:
        lines.append(f"  {name}.headerRow = {bool_literal(child.get('headerRow'))};")
    if child.get("cellRadius") is not None:
        lines.append(f"  {name}.cellRadius = {int(child.get('cellRadius'))};")
    if child.get("padding") is not None:
        lines.append(f"  {name}.padding = {int(child.get('padding'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.table({name}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.table({name}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.table({name});")


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
    if child.get("radius") is not None:
        lines.append(f"  {props}.radius = {int(child.get('radius'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.radioGroup({props}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.radioGroup({props}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.radioGroup({props});")


def emit_popup(child, lines, index):
    styled_keys = ("padding", "align", "maxWidth", "showProgress", "progress", "progressMax", "progressHeight")
    if all(child.get(key) is None for key in styled_keys):
        lines.append(f"  screen.popup({cstr(child.get('message'))});")
        return
    name = f"popup{index}"
    lines.append(f"  freeink::ui::PopupProps {name};")
    lines.append(f"  {name}.message = {cstr(child.get('message'))};")
    lines.append(f"  {name}.text = screen.theme().bodyText;")
    lines.append(f"  {name}.styles = screen.theme().popup;")
    if child.get("align") is not None:
        lines.append(f"  {name}.text.align = {text_align_expr(child.get('align'))};")
    if child.get("maxWidth") is not None:
        lines.append(f"  {name}.maxWidth = {int(child.get('maxWidth'))};")
    if child.get("showProgress") is not None:
        lines.append(f"  {name}.showProgress = {bool_literal(child.get('showProgress'))};")
    if child.get("progress") is not None:
        lines.append(f"  {name}.progress.value = {int(child.get('progress'))};")
    if child.get("progressMax") is not None:
        lines.append(f"  {name}.progress.max = {int(child.get('progressMax'))};")
    if child.get("progressHeight") is not None:
        lines.append(f"  {name}.progressHeight = {int(child.get('progressHeight'))};")
    emit_insets_assignment(lines, name, "padding", child.get("padding"))
    lines.append(f"  screen.popup({name});")


def emit_option_dialog(child, lines, actions, index):
    options = child.get("options", [])
    options_name = f"dialogOptions{index}"
    lines.append(f"  static const freeink::ui::DialogOption {options_name}[] = {{")
    for option in options:
        label = cstr(option.get("label"))
        action = action_expr(option.get("action"), actions)
        value = int(option.get("value", 0))
        enabled = bool_literal(option.get("enabled", True))
        lines.append(f"      {{{label}, {action}, {value}, freeink::ui::StateNormal, {enabled}}},")
    lines.append("  };")
    name = f"optionDialog{index}"
    lines.append(f"  freeink::ui::OptionDialogProps {name};")
    if child.get("title") is not None:
        lines.append(f"  {name}.title = {cstr(child.get('title'))};")
    if child.get("headline") is not None:
        lines.append(f"  {name}.headline = {cstr(child.get('headline'))};")
    if child.get("message") is not None:
        lines.append(f"  {name}.message = {cstr(child.get('message'))};")
    lines.append(f"  {name}.options = {options_name};")
    lines.append(f"  {name}.optionCount = {len(options)};")
    if child.get("buttonHeight") is not None:
        lines.append(f"  {name}.buttonHeight = {int(child.get('buttonHeight'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    if child.get("verticalOptions") is not None:
        lines.append(f"  {name}.verticalOptions = {bool_literal(child.get('verticalOptions'))};")
    if child.get("dimBackground") is not None:
        lines.append(f"  {name}.dimBackground = {bool_literal(child.get('dimBackground'))};")
    emit_insets_assignment(lines, name, "padding", child.get("padding"))
    width = child.get("width")
    if width is not None:
        lines.append(f"  screen.dialog({name}, {int(width)});")
    else:
        lines.append(f"  screen.dialog({name});")


def emit_qwerty_keyboard(child, lines, actions, index):
    name = f"qwertyKeyboard{index}"
    lines.append(f"  freeink::ui::QwertyKeyboardProps {name};")
    if child.get("layout") is not None:
        lines.append(f"  {name}.layout = {keyboard_layout_expr(child.get('layout'))};")
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
    if child.get("keyRadius") is not None:
        lines.append(f"  {name}.keyRadius = {int(child.get('keyRadius'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    emit_insets_assignment(lines, name, "padding", child.get("padding"))
    if child.get("rowGap") is not None:
        lines.append(f"  {name}.rowGap = {int(child.get('rowGap'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.qwertyKeyboard({name}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.qwertyKeyboard({name}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.qwertyKeyboard({name});")


def emit_status_bar(child, lines, index):
    name = f"statusBar{index}"
    lines.append(f"  freeink::ui::StatusBarProps {name};")
    if child.get("title") is not None:
        lines.append(f"  {name}.title = {cstr(child.get('title'))};")
    if child.get("leading") is not None:
        lines.append(f"  {name}.leading = {cstr(child.get('leading'))};")
    if child.get("leadingSecondary") is not None:
        lines.append(f"  {name}.leadingSecondary = {cstr(child.get('leadingSecondary'))};")
    if child.get("trailing") is not None:
        lines.append(f"  {name}.trailing = {cstr(child.get('trailing'))};")
    if child.get("trailingSecondary") is not None:
        lines.append(f"  {name}.trailingSecondary = {cstr(child.get('trailingSecondary'))};")
    if child.get("horizontalPadding") is not None:
        lines.append(f"  {name}.horizontalPadding = {int(child.get('horizontalPadding'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    if child.get("showProgress") is not None:
        lines.append(f"  {name}.showProgress = {bool_literal(child.get('showProgress'))};")
    if child.get("progress") is not None:
        lines.append(f"  {name}.progress.value = {int(child.get('progress'))};")
    if child.get("progressMax") is not None:
        lines.append(f"  {name}.progress.max = {int(child.get('progressMax'))};")
    if child.get("progressHeight") is not None:
        lines.append(f"  {name}.progressHeight = {int(child.get('progressHeight'))};")
    lines.append(f"  {name}.text = screen.theme().smallText;")
    lines.append(f"  screen.status({name}{anchor_arg(child)});")


def emit_book_card(child, lines, actions, index):
    name = f"bookCard{index}"
    lines.append(f"  freeink::ui::BookCardProps {name};")
    if child.get("title") is not None:
        lines.append(f"  {name}.title = {cstr(child.get('title'))};")
    if child.get("author") is not None:
        lines.append(f"  {name}.author = {cstr(child.get('author'))};")
    if child.get("meta") is not None:
        lines.append(f"  {name}.meta = {cstr(child.get('meta'))};")
    if child.get("progress") is not None:
        lines.append(f"  {name}.progress = {int(child.get('progress'))};")
    if child.get("progressMax") is not None:
        lines.append(f"  {name}.progressMax = {int(child.get('progressMax'))};")
    if child.get("action") is not None:
        lines.append(f"  {name}.action = {action_expr(child.get('action'), actions)};")
    if child.get("value") is not None:
        lines.append(f"  {name}.value = {int(child.get('value'))};")
    if child.get("gap") is not None:
        lines.append(f"  {name}.gap = {int(child.get('gap'))};")
    if child.get("textGap") is not None:
        lines.append(f"  {name}.textGap = {int(child.get('textGap'))};")
    emit_insets_assignment(lines, name, "padding", child.get("padding"))
    lines.append(f"  {name}.titleText = screen.theme().bodyText;")
    lines.append(f"  {name}.authorText = screen.theme().smallText;")
    lines.append(f"  {name}.metaText = screen.theme().smallText;")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.bookCard({name}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.bookCard({name}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.bookCard({name});")


def emit_text_area(child, lines, index):
    name = f"textArea{index}"
    lines.append(f"  freeink::ui::TextAreaProps {name};")
    if child.get("text") is not None:
        lines.append(f"  {name}.text = {cstr(child.get('text'))};")
    if child.get("cursor") is not None:
        lines.append(f"  {name}.cursor = {int(child.get('cursor'))};")
    if child.get("topLine") is not None:
        lines.append(f"  {name}.topLine = {int(child.get('topLine'))};")
    if child.get("showCaret") is not None:
        lines.append(f"  {name}.showCaret = {bool_literal(child.get('showCaret'))};")
    if child.get("selStart") is not None:
        lines.append(f"  {name}.selStart = {int(child.get('selStart'))};")
    if child.get("selEnd") is not None:
        lines.append(f"  {name}.selEnd = {int(child.get('selEnd'))};")
    height = child.get("height")
    if height is not None:
        lines.append(f"  screen.textArea({name}, {int(height)}{anchor_arg(child)});")
    elif child.get("anchor") is not None:
        lines.append(f"  screen.textArea({name}, 0, {anchor_expr(child)});")
    else:
        lines.append(f"  screen.textArea({name});")


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
    content_margin = insets_expr(schema.get("contentMargin"))
    if content_margin is not None:
        lines.append(f"  screen.setContentMargin({content_margin});")
    safe_area = insets_expr(schema.get("safeArea"))
    if safe_area is not None:
        lines.append(f"  // Device safe area for this design: {safe_area}. Apply it to DeviceContext.safeArea.")

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
            emit_button(child, lines, actions, index)
        elif kind == "settingRow":
            emit_setting_row(child, lines, actions, index)
        elif kind == "toggleRow":
            emit_toggle_row(child, lines, actions, index)
        elif kind == "stepperRow":
            emit_stepper_row(child, lines, actions, index)
        elif kind == "checkbox":
            emit_checkbox(child, lines, actions, index)
        elif kind == "slider":
            emit_slider(child, lines, actions, index)
        elif kind == "dropdown":
            emit_dropdown(child, lines, actions, index)
        elif kind == "table":
            emit_table(child, lines, index)
        elif kind == "radioGroup":
            emit_radio_group(child, lines, actions, index)
        elif kind == "spacer":
            emit_spacer(child, lines)
        elif kind == "popup":
            emit_popup(child, lines, index)
        elif kind == "optionDialog":
            emit_option_dialog(child, lines, actions, index)
        elif kind == "qwertyKeyboard":
            emit_qwerty_keyboard(child, lines, actions, index)
        elif kind == "statusBar":
            emit_status_bar(child, lines, index)
        elif kind == "bookCard":
            emit_book_card(child, lines, actions, index)
        elif kind == "textArea":
            emit_text_area(child, lines, index)
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
