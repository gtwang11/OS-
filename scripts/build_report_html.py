#!/usr/bin/env python3
"""Build a polished HTML version of docs/实验报告.md."""

from __future__ import annotations

import html
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MD_PATH = ROOT / "docs" / "实验报告.md"
HTML_PATH = ROOT / "docs" / "实验报告.html"

COVER_LABELS = {
    "课程名称",
    "实验题目",
    "学生姓名",
    "学号",
    "班级",
    "代码 URL",
    "本地工程路径",
}

CHART_TITLES = {
    "平均等待时间条形图：": True,
    "缺页率图：": True,
    "吞吐量图：": False,
}


def inline_markup(text: str) -> str:
    parts = re.split(r"(`[^`]*`)", text)
    out: list[str] = []
    for part in parts:
        if part.startswith("`") and part.endswith("`"):
            code = html.escape(part[1:-1])
            out.append(f"<code>{code}</code>")
            continue
        escaped = html.escape(part)
        escaped = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", escaped)
        out.append(escaped)
    return "".join(out)


def extract_cover_fields(markdown: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in markdown.splitlines():
        stripped = line.strip()
        if stripped.startswith("## "):
            break
        match = re.match(r"^([^：:]+)[：:]\s*(.+?)\s*$", stripped)
        if match and match.group(1) in COVER_LABELS:
            fields[match.group(1)] = match.group(2)
    return fields


def split_table_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [cell.strip() for cell in line.split("|")]


def is_table_separator(line: str) -> bool:
    cells = split_table_row(line)
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def render_table(lines: list[str]) -> str:
    rows = [split_table_row(line) for line in lines if not is_table_separator(line)]
    if not rows:
        return ""
    header, body = rows[0], rows[1:]
    max_cols = max(len(row) for row in rows)

    def pad(row: list[str]) -> list[str]:
        return row + [""] * (max_cols - len(row))

    head_html = "".join(f"<th>{inline_markup(cell)}</th>" for cell in pad(header))
    body_rows = []
    for row in body:
        cells = "".join(f"<td>{inline_markup(cell)}</td>" for cell in pad(row))
        body_rows.append(f"<tr>{cells}</tr>")
    return (
        '<div class="table-wrap"><table>'
        f"<thead><tr>{head_html}</tr></thead>"
        f"<tbody>{''.join(body_rows)}</tbody>"
        "</table></div>"
    )


def parse_chart_lines(text: str) -> list[tuple[str, float, str]]:
    rows: list[tuple[str, float, str]] = []
    for raw in text.splitlines():
        if "|" not in raw:
            continue
        left = raw.split("|", 1)[0].strip()
        match = re.match(r"^(.+?)\s+([0-9]+(?:\.[0-9]+)?)(M?)$", left)
        if not match:
            continue
        label = match.group(1).strip().replace("_", " ")
        value = float(match.group(2))
        suffix = match.group(3)
        value_text = f"{value:g}{suffix}"
        if suffix == "M":
            value_text += " ops/s"
        rows.append((label, value, value_text))
    return rows


def render_chart(title: str, code_text: str, lower_better: bool) -> str:
    rows = parse_chart_lines(code_text)
    if not rows:
        return render_code(code_text, "text")
    max_value = max(value for _, value, _ in rows) or 1.0
    best_value = min(value for _, value, _ in rows) if lower_better else max(value for _, value, _ in rows)
    rendered_rows = []
    for label, value, value_text in rows:
        width = max(1.0, value / max_value * 100.0)
        best_class = " best" if abs(value - best_value) < 1e-9 else ""
        rendered_rows.append(
            '<div class="chart-row{best_class}">'
            '<div class="chart-label">{label}</div>'
            '<div class="chart-track"><div class="chart-bar" style="width:{width:.2f}%"></div></div>'
            '<div class="chart-value">{value_text}</div>'
            "</div>".format(
                best_class=best_class,
                label=html.escape(label),
                width=width,
                value_text=html.escape(value_text),
            )
        )
    return (
        '<figure class="chart">'
        f'<figcaption>{html.escape(title)}</figcaption>'
        f"{''.join(rendered_rows)}"
        "</figure>"
    )


def render_code(code_text: str, language: str) -> str:
    lang = language.strip() or "text"
    safe_lang = re.sub(r"[^A-Za-z0-9_-]+", "", lang) or "text"
    return (
        '<div class="code-block">'
        f'<div class="code-title">{html.escape(safe_lang)}</div>'
        f'<pre><code class="language-{html.escape(safe_lang)}">{html.escape(code_text.rstrip())}</code></pre>'
        "</div>"
    )


def render_list(lines: list[str], ordered: bool) -> str:
    tag = "ol" if ordered else "ul"
    items = []
    pattern = r"^\s*\d+\.\s+(.+)$" if ordered else r"^\s*[-*]\s+(.+)$"
    for line in lines:
        match = re.match(pattern, line)
        if match:
            items.append(f"<li>{inline_markup(match.group(1))}</li>")
    return f"<{tag}>{''.join(items)}</{tag}>"


def render_cover(fields: dict[str, str]) -> str:
    title = "操作系统课程设计实验报告"
    subtitle = fields.get("实验题目", "操作系统核心机制模拟与 Linux 内核扩展实践")
    ordered_labels = ["课程名称", "学生姓名", "学号", "班级", "代码 URL", "本地工程路径"]
    items = []
    for label in ordered_labels:
        value = fields.get(label, "__________")
        items.append(
            "<div>"
            f"<dt>{html.escape(label)}</dt>"
            f"<dd>{inline_markup(value)}</dd>"
            "</div>"
        )
    return f"""
<section class="cover">
  <p class="eyebrow">2026 · 操作系统课程设计</p>
  <h1>{html.escape(title)}</h1>
  <p class="subtitle">{html.escape(subtitle)}</p>
  <dl class="cover-meta">
    {''.join(items)}
  </dl>
  <p class="cover-note">报告内容依据本地工程实现、自动化测试输出和实际内核验证结果整理；未上传代码仓库前不填写虚假 URL。</p>
</section>
"""


def render_markdown(markdown: str) -> str:
    lines = markdown.splitlines()
    out: list[str] = []
    paragraph: list[str] = []
    skipped_title = False
    seen_first_section = False
    pending_chart: tuple[str, bool] | None = None
    i = 0

    def flush_paragraph() -> None:
        if not paragraph:
            return
        text = " ".join(part.strip() for part in paragraph if part.strip())
        if text:
            out.append(f"<p>{inline_markup(text)}</p>")
        paragraph.clear()

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if stripped.startswith("# ") and not skipped_title:
            skipped_title = True
            i += 1
            continue

        if not seen_first_section:
            match = re.match(r"^([^：:]+)[：:]\s*(.+?)\s*$", stripped)
            if match and match.group(1) in COVER_LABELS:
                i += 1
                continue

        if stripped in CHART_TITLES:
            flush_paragraph()
            pending_chart = (stripped.rstrip("："), CHART_TITLES[stripped])
            i += 1
            continue

        if stripped.startswith("```"):
            flush_paragraph()
            language = stripped[3:].strip() or "text"
            i += 1
            code_lines: list[str] = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i])
                i += 1
            code_text = "\n".join(code_lines)
            if pending_chart:
                out.append(render_chart(pending_chart[0], code_text, pending_chart[1]))
                pending_chart = None
            else:
                out.append(render_code(code_text, language))
            i += 1
            continue

        if stripped.startswith("|") and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
            flush_paragraph()
            table_lines = [line, lines[i + 1]]
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                table_lines.append(lines[i])
                i += 1
            out.append(render_table(table_lines))
            continue

        heading = re.match(r"^(#{2,4})\s+(.+)$", stripped)
        if heading:
            seen_first_section = True
            flush_paragraph()
            level = len(heading.group(1))
            text = inline_markup(heading.group(2))
            out.append(f"<h{level}>{text}</h{level}>")
            i += 1
            continue

        if re.match(r"^\s*[-*]\s+", line):
            flush_paragraph()
            list_lines = []
            while i < len(lines) and re.match(r"^\s*[-*]\s+", lines[i]):
                list_lines.append(lines[i])
                i += 1
            out.append(render_list(list_lines, ordered=False))
            continue

        if re.match(r"^\s*\d+\.\s+", line):
            flush_paragraph()
            list_lines = []
            while i < len(lines) and re.match(r"^\s*\d+\.\s+", lines[i]):
                list_lines.append(lines[i])
                i += 1
            out.append(render_list(list_lines, ordered=True))
            continue

        if not stripped:
            flush_paragraph()
            i += 1
            continue

        paragraph.append(stripped)
        i += 1

    flush_paragraph()
    return "\n".join(out)


def build_html(markdown: str) -> str:
    fields = extract_cover_fields(markdown)
    body = render_markdown(markdown)
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>操作系统课程设计实验报告</title>
  <style>
    :root {{
      --fg: #24292f;
      --muted: #57606a;
      --border: #d0d7de;
      --soft-border: #eaeef2;
      --accent: #0969da;
      --accent-dark: #0a3069;
      --success: #1a7f37;
      --canvas: #f6f8fa;
      --code-bg: #f6f8fa;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      color: var(--fg);
      background: var(--canvas);
      font: 15px/1.72 -apple-system, BlinkMacSystemFont, "Segoe UI", "Noto Sans CJK SC", "Microsoft YaHei", sans-serif;
    }}
    main {{
      max-width: 980px;
      margin: 32px auto;
      background: #fff;
      border: 1px solid var(--border);
      border-radius: 8px;
      overflow: hidden;
    }}
    article {{ padding: 24px 48px 52px; }}
    .cover {{
      padding: 56px 56px 44px;
      border-bottom: 1px solid var(--border);
      background: linear-gradient(180deg, #ffffff 0%, #f6f8fa 100%);
    }}
    .eyebrow {{
      margin: 0 0 12px;
      color: var(--muted);
      font-size: 13px;
      letter-spacing: .02em;
    }}
    h1 {{
      margin: 0;
      color: #0b1f33;
      font-size: 36px;
      line-height: 1.22;
      letter-spacing: 0;
    }}
    .subtitle {{
      margin: 12px 0 26px;
      color: var(--accent-dark);
      font-size: 18px;
    }}
    .cover-meta {{
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 14px 24px;
      margin: 0;
    }}
    .cover-meta div {{
      min-height: 52px;
      padding: 0 0 12px;
      border-bottom: 1px solid var(--border);
    }}
    .cover-meta dt {{
      color: var(--muted);
      font-size: 12px;
      font-weight: 600;
      margin-bottom: 4px;
    }}
    .cover-meta dd {{
      margin: 0;
      color: var(--fg);
      font-weight: 600;
      overflow-wrap: anywhere;
    }}
    .cover-note {{
      margin: 28px 0 0;
      color: var(--muted);
      font-size: 13px;
    }}
    h2 {{
      margin: 32px 0 16px;
      padding-bottom: 8px;
      border-bottom: 1px solid var(--border);
      color: #0b1f33;
      font-size: 24px;
      line-height: 1.3;
      letter-spacing: 0;
    }}
    h3 {{
      margin: 26px 0 10px;
      color: #1f3a53;
      font-size: 18px;
      line-height: 1.35;
      letter-spacing: 0;
    }}
    h4 {{
      margin: 20px 0 8px;
      color: #2b4b68;
      font-size: 15px;
      line-height: 1.35;
      letter-spacing: 0;
    }}
    p {{ margin: 8px 0 14px; }}
    ul, ol {{ margin: 8px 0 16px; padding-left: 26px; }}
    li {{ margin: 4px 0; }}
    code {{
      padding: .16em .36em;
      border-radius: 6px;
      background: #eff3f6;
      color: #0a3069;
      font-family: ui-monospace, SFMono-Regular, "SF Mono", Consolas, "Liberation Mono", monospace;
      font-size: 0.92em;
    }}
    .code-block {{
      margin: 14px 0 18px;
      overflow: hidden;
      border: 1px solid var(--border);
      border-radius: 6px;
      background: var(--code-bg);
    }}
    .code-title {{
      padding: 6px 12px;
      border-bottom: 1px solid var(--border);
      color: var(--muted);
      font: 12px/1.4 ui-monospace, SFMono-Regular, "SF Mono", Consolas, "Liberation Mono", monospace;
      background: #fff;
    }}
    pre {{
      margin: 0;
      padding: 14px 16px;
      overflow-x: auto;
      color: var(--fg);
      font: 12.5px/1.55 ui-monospace, SFMono-Regular, "SF Mono", Consolas, "Liberation Mono", monospace;
      white-space: pre;
    }}
    pre code {{
      padding: 0;
      border-radius: 0;
      background: transparent;
      color: inherit;
      font: inherit;
    }}
    .table-wrap {{ overflow-x: auto; margin: 14px 0 20px; }}
    table {{
      width: 100%;
      border-spacing: 0;
      border-collapse: collapse;
      font-size: 13px;
    }}
    th, td {{
      padding: 7px 9px;
      border: 1px solid var(--border);
      vertical-align: top;
    }}
    th {{
      background: #f6f8fa;
      color: #0a3069;
      font-weight: 700;
      white-space: nowrap;
    }}
    tbody tr:nth-child(even) td {{ background: #fbfdff; }}
    .chart {{
      margin: 16px 0 22px;
      padding: 16px;
      border: 1px solid var(--border);
      border-radius: 6px;
      background: #fff;
    }}
    .chart figcaption {{
      margin: 0 0 12px;
      color: #0b1f33;
      font-weight: 700;
    }}
    .chart-row {{
      display: grid;
      grid-template-columns: 150px minmax(220px, 1fr) 112px;
      gap: 12px;
      align-items: center;
      margin: 8px 0;
    }}
    .chart-label {{
      color: var(--fg);
      font-size: 13px;
      text-align: right;
      white-space: nowrap;
    }}
    .chart-track {{
      height: 12px;
      overflow: hidden;
      border-radius: 999px;
      background: #eaeef2;
    }}
    .chart-bar {{
      height: 100%;
      border-radius: 999px;
      background: var(--accent);
    }}
    .chart-row.best .chart-bar {{ background: var(--success); }}
    .chart-value {{
      color: var(--muted);
      font: 12px/1.3 ui-monospace, SFMono-Regular, "SF Mono", Consolas, "Liberation Mono", monospace;
    }}
    @media (max-width: 760px) {{
      main {{ margin: 0; border: 0; border-radius: 0; }}
      article, .cover {{ padding-left: 22px; padding-right: 22px; }}
      .cover-meta {{ grid-template-columns: 1fr; }}
      .chart-row {{ grid-template-columns: 1fr; gap: 5px; }}
      .chart-label {{ text-align: left; }}
    }}
    @media print {{
      body {{ background: #fff; }}
      main {{ margin: 0; border: 0; border-radius: 0; }}
      .code-block, .chart, table {{ break-inside: avoid; }}
    }}
  </style>
</head>
<body>
<main>
{render_cover(fields)}
<article>
{body}
</article>
</main>
</body>
</html>
"""


def main() -> int:
    markdown = MD_PATH.read_text(encoding="utf-8")
    HTML_PATH.write_text(build_html(markdown), encoding="utf-8")
    print(f"HTML written: {HTML_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
