#!/usr/bin/env python3
"""Build the OS course design PDF directly from docs/实验报告.md."""

from __future__ import annotations

import html
import re
import sys
import textwrap
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.pdfbase.cidfonts import UnicodeCIDFont
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    Flowable,
    PageBreak,
    Paragraph,
    Preformatted,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)


ROOT = Path(__file__).resolve().parents[1]
MD_PATH = ROOT / "docs" / "实验报告.md"
PDF_PATH = ROOT / "docs" / "实验报告.pdf"

BLUE = colors.HexColor("#2f6f9f")
DARK = colors.HexColor("#102033")
TEXT = colors.HexColor("#17202a")
LIGHT_BLUE = colors.HexColor("#eaf2fa")
PALE_BLUE = colors.HexColor("#f5f9fd")
BORDER = colors.HexColor("#c9d8e8")
CODE_BG = colors.HexColor("#f7fbff")
GITHUB_CODE_BG = colors.HexColor("#f6f8fa")
GITHUB_BORDER = colors.HexColor("#d0d7de")
COVER_LABELS = {
    "课程名称",
    "实验题目",
    "学生姓名",
    "学号",
    "班级",
    "代码 URL",
    "本地工程路径",
}
CODE_FONT_NAME = "DejaVuSansMono"
CODE_FONT_PATH = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")


def register_fonts() -> tuple[str, str]:
    # STSong-Light is a standard ReportLab CID font. It avoids the missing
    # Latin/underscore glyph problem seen with some fallback TTF fonts in this
    # environment, while still supporting Chinese text.
    pdfmetrics.registerFont(UnicodeCIDFont("STSong-Light"))
    pdfmetrics.registerFontFamily(
        "STSong-Light",
        normal="STSong-Light",
        bold="STSong-Light",
        italic="STSong-Light",
        boldItalic="STSong-Light",
    )
    code_font = "STSong-Light"
    if CODE_FONT_PATH.exists():
        pdfmetrics.registerFont(TTFont(CODE_FONT_NAME, str(CODE_FONT_PATH)))
        code_font = CODE_FONT_NAME
    return "STSong-Light", code_font


def make_styles(font: str, code_font: str) -> dict[str, ParagraphStyle]:
    base = getSampleStyleSheet()
    styles: dict[str, ParagraphStyle] = {}
    styles["title"] = ParagraphStyle(
        "Title",
        parent=base["Title"],
        fontName=font,
        fontSize=28,
        leading=36,
        textColor=DARK,
        alignment=TA_CENTER,
        spaceAfter=16,
    )
    styles["subtitle"] = ParagraphStyle(
        "Subtitle",
        parent=base["Normal"],
        fontName=font,
        fontSize=13,
        leading=20,
        textColor=BLUE,
        alignment=TA_CENTER,
        spaceAfter=22,
    )
    styles["cover_meta"] = ParagraphStyle(
        "CoverMeta",
        parent=base["Normal"],
        fontName=font,
        fontSize=11,
        leading=17,
        textColor=TEXT,
    )
    styles["h2"] = ParagraphStyle(
        "Heading2",
        parent=base["Heading1"],
        fontName=font,
        fontSize=17,
        leading=24,
        textColor=DARK,
        borderColor=BLUE,
        borderWidth=0,
        leftIndent=0,
        spaceBefore=14,
        spaceAfter=10,
        keepWithNext=True,
    )
    styles["h3"] = ParagraphStyle(
        "Heading3",
        parent=base["Heading2"],
        fontName=font,
        fontSize=13.5,
        leading=20,
        textColor=colors.HexColor("#2b4b68"),
        spaceBefore=10,
        spaceAfter=7,
        keepWithNext=True,
    )
    styles["h4"] = ParagraphStyle(
        "Heading4",
        parent=base["Heading3"],
        fontName=font,
        fontSize=11.5,
        leading=17,
        textColor=colors.HexColor("#385a73"),
        spaceBefore=7,
        spaceAfter=5,
        keepWithNext=True,
    )
    styles["body"] = ParagraphStyle(
        "Body",
        parent=base["BodyText"],
        fontName=font,
        fontSize=10.4,
        leading=17,
        textColor=TEXT,
        alignment=TA_LEFT,
        spaceAfter=6,
    )
    styles["bullet"] = ParagraphStyle(
        "Bullet",
        parent=styles["body"],
        leftIndent=18,
        firstLineIndent=-11,
        spaceAfter=4,
    )
    styles["code"] = ParagraphStyle(
        "Code",
        parent=base["Code"],
        fontName=font,
        fontSize=8.4,
        leading=12,
        textColor=colors.HexColor("#162434"),
    )
    styles["code_mono"] = ParagraphStyle(
        "CodeMono",
        parent=styles["code"],
        fontName=code_font,
        fontSize=8.0,
        leading=11.5,
    )
    styles["table"] = ParagraphStyle(
        "TableCell",
        parent=styles["body"],
        fontName=font,
        fontSize=8.5,
        leading=11.5,
        spaceAfter=0,
    )
    styles["table_header"] = ParagraphStyle(
        "TableHeader",
        parent=styles["table"],
        fontName=font,
        fontSize=8.7,
        leading=12,
        textColor=colors.HexColor("#15324b"),
        alignment=TA_CENTER,
    )
    return styles


def inline_markup(text: str) -> str:
    """Convert a small Markdown inline subset to ReportLab-safe markup."""
    parts = re.split(r"(`[^`]*`)", text)
    out: list[str] = []
    for part in parts:
        if part.startswith("`") and part.endswith("`"):
            code = html.escape(part[1:-1])
            out.append(f'<font color="#0f3d5f">{code}</font>')
            continue
        escaped = html.escape(part)
        escaped = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", escaped)
        out.append(escaped)
    return "".join(out)


def para(text: str, style: ParagraphStyle) -> Paragraph:
    return Paragraph(inline_markup(text), style)


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


def add_cover(story: list, styles: dict[str, ParagraphStyle], cover_fields: dict[str, str]) -> None:
    story.append(Spacer(1, 1.5 * cm))
    story.append(Paragraph("操作系统课程设计实验报告", styles["title"]))
    story.append(
        Paragraph(
            cover_fields.get("实验题目", "操作系统核心机制模拟与 Linux 内核扩展实践"),
            styles["subtitle"],
        )
    )

    story.append(Spacer(1, 1.2 * cm))
    for label in ["课程名称", "学生姓名", "学号", "班级", "代码 URL", "本地工程路径"]:
        value = cover_fields.get(label, "__________")
        story.append(
            Paragraph(
                inline_markup(f"**{label}：**{value}"),
                styles["cover_meta"],
            )
        )
        story.append(Spacer(1, 0.18 * cm))
    story.append(Spacer(1, 1.0 * cm))
    story.append(
        Paragraph(
            "报告内容依据本地工程实现、自动化测试输出和实际内核验证结果整理；未上传代码仓库前不填写虚假 URL。",
            styles["body"],
        )
    )
    story.append(
        Paragraph(
            "关键词：处理机调度、内存管理、进程同步、miniFS、Linux 内核模块、系统调用扩展、性能优化",
            styles["body"],
        )
    )
    story.append(PageBreak())


def split_table_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [cell.strip() for cell in line.split("|")]


def is_table_separator(line: str) -> bool:
    cells = split_table_row(line)
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", c.strip()) for c in cells)


def table_col_widths(rows: list[list[str]], available_width: float) -> list[float]:
    n = max(len(r) for r in rows)
    if n == 1:
        return [available_width]
    if n == 2:
        return [available_width * 0.32, available_width * 0.68]
    if n == 3:
        return [available_width * 0.20, available_width * 0.43, available_width * 0.37]
    if n == 4:
        return [available_width * 0.22, available_width * 0.22, available_width * 0.25, available_width * 0.31]
    if n == 5:
        return [available_width * 0.22] + [available_width * 0.195] * 4
    if n == 6:
        return [available_width * 0.20] + [available_width * 0.16] * 5
    return [available_width / n] * n


def table_flowable(
    raw_rows: list[str],
    styles: dict[str, ParagraphStyle],
    available_width: float,
) -> Table:
    rows = [split_table_row(r) for r in raw_rows if not is_table_separator(r)]
    if not rows:
        rows = [[""]]
    max_cols = max(len(r) for r in rows)
    normalized = [r + [""] * (max_cols - len(r)) for r in rows]
    data = []
    for row_idx, row in enumerate(normalized):
        row_style = styles["table_header"] if row_idx == 0 else styles["table"]
        data.append([Paragraph(inline_markup(cell), row_style) for cell in row])
    table = Table(
        data,
        colWidths=table_col_widths(normalized, available_width),
        repeatRows=1 if len(data) > 1 else 0,
        hAlign="LEFT",
    )
    table.setStyle(
        TableStyle(
            [
                ("GRID", (0, 0), (-1, -1), 0.45, BORDER),
                ("BACKGROUND", (0, 0), (-1, 0), LIGHT_BLUE),
                ("TEXTCOLOR", (0, 0), (-1, 0), colors.HexColor("#15324b")),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ("LEFTPADDING", (0, 0), (-1, -1), 5),
                ("RIGHTPADDING", (0, 0), (-1, -1), 5),
                ("TOPPADDING", (0, 0), (-1, -1), 5),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
            ]
        )
    )
    return table


class HorizontalBarChart(Flowable):
    def __init__(
        self,
        title: str,
        data: list[tuple[str, float, str]],
        width: float,
        lower_better: bool,
    ) -> None:
        super().__init__()
        self.title = title
        self.data = data
        self.width = width
        self.lower_better = lower_better
        self.height = 42 + len(data) * 22

    def wrap(self, avail_width: float, avail_height: float) -> tuple[float, float]:
        self.width = min(self.width, avail_width)
        return self.width, self.height

    def draw(self) -> None:
        c = self.canv
        c.saveState()
        c.setStrokeColor(GITHUB_BORDER)
        c.setFillColor(colors.white)
        c.roundRect(0, 0, self.width, self.height, 4, stroke=1, fill=1)

        c.setFont("STSong-Light", 10)
        c.setFillColor(DARK)
        c.drawString(12, self.height - 18, self.title)

        if not self.data:
            c.restoreState()
            return

        label_w = 105
        value_w = 72
        chart_x = 12 + label_w
        chart_w = self.width - label_w - value_w - 30
        top_y = self.height - 40
        max_value = max(value for _, value, _ in self.data) or 1.0
        best_value = min(v for _, v, _ in self.data) if self.lower_better else max(v for _, v, _ in self.data)

        c.setStrokeColor(colors.HexColor("#edf2f7"))
        for ratio in (0.25, 0.5, 0.75, 1.0):
            x = chart_x + chart_w * ratio
            c.line(x, 16, x, top_y + 10)

        for idx, (label, value, value_text) in enumerate(self.data):
            y = top_y - idx * 22
            bar_w = chart_w * value / max_value
            is_best = abs(value - best_value) < 1e-9
            fill = colors.HexColor("#1f9d72") if is_best else BLUE

            c.setFillColor(TEXT)
            c.setFont("STSong-Light", 8.8)
            c.drawRightString(chart_x - 8, y + 2, label)

            c.setFillColor(colors.HexColor("#edf2f7"))
            c.roundRect(chart_x, y - 2, chart_w, 10, 2, stroke=0, fill=1)
            c.setFillColor(fill)
            c.roundRect(chart_x, y - 2, max(1.0, bar_w), 10, 2, stroke=0, fill=1)

            c.setFillColor(TEXT)
            c.drawString(chart_x + chart_w + 8, y + 2, value_text)

        c.restoreState()


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


def code_flowable(
    text: str,
    styles: dict[str, ParagraphStyle],
    available_width: float,
    language: str = "text",
) -> Table:
    wrapped_lines: list[str] = []
    for line in text.rstrip("\n").splitlines():
        if not line:
            wrapped_lines.append("")
            continue
        # Keep logs and command lines readable on A4 while preserving indentation.
        chunks = textwrap.wrap(
            line,
            width=92,
            replace_whitespace=False,
            drop_whitespace=False,
            break_long_words=False,
            break_on_hyphens=False,
        )
        wrapped_lines.extend(chunks or [""])
    safe_text = "\n".join(wrapped_lines)
    code_style = styles["code_mono"] if safe_text.isascii() else styles["code"]
    pre = Preformatted(safe_text, code_style, maxLineLength=92)
    lang = language.strip() or "text"
    header = Paragraph(
        f'<font color="#57606a">{html.escape(lang)}</font>',
        ParagraphStyle(
            "CodeHeader",
            parent=styles["table"],
            fontName="STSong-Light",
            fontSize=8,
            leading=10,
        ),
    )
    table = Table([[header], [pre]], colWidths=[available_width], hAlign="LEFT")
    table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, -1), GITHUB_CODE_BG),
                ("BOX", (0, 0), (-1, -1), 0.45, GITHUB_BORDER),
                ("LINEBELOW", (0, 0), (-1, 0), 0.45, GITHUB_BORDER),
                ("LEFTPADDING", (0, 0), (-1, -1), 7),
                ("RIGHTPADDING", (0, 0), (-1, -1), 7),
                ("TOPPADDING", (0, 0), (-1, 0), 4),
                ("BOTTOMPADDING", (0, 0), (-1, 0), 4),
                ("TOPPADDING", (0, 1), (-1, 1), 7),
                ("BOTTOMPADDING", (0, 1), (-1, 1), 7),
            ]
        )
    )
    return table


def flush_paragraph(buf: list[str], story: list, styles: dict[str, ParagraphStyle]) -> None:
    if not buf:
        return
    text = " ".join(part.strip() for part in buf if part.strip())
    if text:
        story.append(para(text, styles["body"]))
    buf.clear()


def build_story(
    markdown: str,
    styles: dict[str, ParagraphStyle],
    available_width: float,
    cover_fields: dict[str, str],
) -> list:
    story: list = []
    add_cover(story, styles, cover_fields)

    lines = markdown.splitlines()
    i = 0
    para_buf: list[str] = []
    skipped_title = False
    seen_first_section = False
    pending_chart_title: str | None = None

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

        if stripped in {"平均等待时间条形图：", "缺页率图：", "吞吐量图："}:
            flush_paragraph(para_buf, story, styles)
            pending_chart_title = stripped.rstrip("：")
            i += 1
            continue

        if stripped.startswith("```"):
            flush_paragraph(para_buf, story, styles)
            language = stripped[3:].strip() or "text"
            i += 1
            code_lines: list[str] = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i])
                i += 1
            code_text = "\n".join(code_lines)
            if pending_chart_title:
                chart_data = parse_chart_lines(code_text)
                if chart_data:
                    lower_better = pending_chart_title in {"平均等待时间条形图", "缺页率图"}
                    story.append(
                        HorizontalBarChart(
                            pending_chart_title,
                            chart_data,
                            available_width,
                            lower_better=lower_better,
                        )
                    )
                    pending_chart_title = None
                else:
                    story.append(code_flowable(code_text, styles, available_width, language))
            else:
                story.append(code_flowable(code_text, styles, available_width, language))
            story.append(Spacer(1, 6))
            i += 1
            continue

        if stripped.startswith("|") and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
            flush_paragraph(para_buf, story, styles)
            table_lines = [line, lines[i + 1]]
            i += 2
            while i < len(lines) and lines[i].strip().startswith("|"):
                table_lines.append(lines[i])
                i += 1
            story.append(table_flowable(table_lines, styles, available_width))
            story.append(Spacer(1, 6))
            continue

        if not stripped:
            flush_paragraph(para_buf, story, styles)
            story.append(Spacer(1, 3))
            i += 1
            continue

        heading = re.match(r"^(#{2,4})\s+(.+)$", stripped)
        if heading:
            seen_first_section = True
            flush_paragraph(para_buf, story, styles)
            level = len(heading.group(1))
            text = heading.group(2)
            if level == 2:
                story.append(Spacer(1, 5))
                story.append(para(text, styles["h2"]))
            elif level == 3:
                story.append(para(text, styles["h3"]))
            else:
                story.append(para(text, styles["h4"]))
            i += 1
            continue

        bullet = re.match(r"^(\s*)[-*]\s+(.+)$", line)
        numbered = re.match(r"^(\s*)(\d+)\.\s+(.+)$", line)
        if bullet or numbered:
            flush_paragraph(para_buf, story, styles)
            indent_spaces = len((bullet or numbered).group(1))
            if bullet:
                text = bullet.group(2)
                prefix = "-"
            else:
                text = numbered.group(3)
                prefix = f"{numbered.group(2)}."
            style = ParagraphStyle(
                f"ListIndent{indent_spaces}",
                parent=styles["bullet"],
                leftIndent=18 + indent_spaces * 3,
                firstLineIndent=-12,
            )
            story.append(Paragraph(f"{prefix} {inline_markup(text)}", style))
            i += 1
            continue

        if stripped.startswith(">"):
            flush_paragraph(para_buf, story, styles)
            quote = stripped.lstrip(">").strip()
            story.append(para(quote, styles["body"]))
            i += 1
            continue

        para_buf.append(stripped)
        i += 1

    flush_paragraph(para_buf, story, styles)
    return story


def draw_footer(canvas, doc) -> None:
    canvas.saveState()
    canvas.setFont("STSong-Light", 8)
    canvas.setFillColor(colors.HexColor("#60788e"))
    canvas.drawRightString(A4[0] - doc.rightMargin, 1.0 * cm, str(doc.page))
    canvas.restoreState()


def main() -> int:
    font, code_font = register_fonts()
    styles = make_styles(font, code_font)
    markdown = MD_PATH.read_text(encoding="utf-8")
    cover_fields = extract_cover_fields(markdown)

    doc = SimpleDocTemplate(
        str(PDF_PATH),
        pagesize=A4,
        leftMargin=1.65 * cm,
        rightMargin=1.65 * cm,
        topMargin=1.65 * cm,
        bottomMargin=1.55 * cm,
        title="操作系统课程设计实验报告",
        author="OS course design",
    )
    story = build_story(markdown, styles, doc.width, cover_fields)
    doc.build(story, onFirstPage=draw_footer, onLaterPages=draw_footer)
    print(f"PDF written: {PDF_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
