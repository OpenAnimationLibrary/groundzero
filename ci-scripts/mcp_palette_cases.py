"""Qualify native palette pages, membership, and independent animated copies."""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


DRAWING = {"number": 10, "suffix": "a"}
COLOR = {"r": 35, "g": 70, "b": 105, "a": 200}
SECOND_COLOR = {"r": 180, "g": 40, "b": 80, "a": 255}


def _pages(test: NativeAcceptance, document: str, level_id: str) -> dict[str, Any]:
    return test.success("palette.pages", {"document_id": document, "level_id": level_id})


def _styles(test: NativeAcceptance, document: str, level_id: str) -> list[dict[str, Any]]:
    return test.success("palette.inspect", {"document_id": document, "level_id": level_id})[
        "styles"
    ]


def _fixture(test: NativeAcceptance, image_format: str) -> tuple[str, str]:
    document = test.create("Palette lifecycle")
    level = test.mutate("level.create", document, name="Artwork", format=image_format)
    level_id = level["level_id"]
    dimensions = {} if image_format == "vector" else {"width": 2, "height": 2, "dpi": 120}
    test.mutate(
        "drawing.create",
        document,
        level_id=level_id,
        drawing_id=DRAWING,
        format=image_format,
        **dimensions,
    )
    for name in ("Paint", "Ink"):
        test.mutate("palette.create_style", document, level_id=level_id, name=name, color=COLOR)
    for frame, color in ((0, COLOR), (12, SECOND_COLOR)):
        test.mutate(
            "palette.update_style",
            document,
            level_id=level_id,
            style_id=2,
            keyframe=frame,
            color=color,
        )
    _make_artwork(test, document, level_id, image_format)
    column = test.mutate("column.create", document, name="Held artwork")
    test.mutate(
        "column.expose",
        document,
        level_id=level_id,
        drawing_id=DRAWING,
        column_id=column["column_id"],
        start_frame=0,
        length=2,
    )
    return document, level_id


def _make_artwork(
    test: NativeAcceptance,
    document: str,
    level_id: str,
    image_format: str,
) -> None:
    target = {"level_id": level_id, "drawing_id": DRAWING, "sharing": "all_exposures"}
    if image_format == "vector":
        points = [{"x": x, "y": 0, "thickness": 1} for x in (-4, 0, 4)]
        test.mutate(
            "vector.edit",
            document,
            **target,
            coordinate_space="drawing_native",
            edits=[{"action": "add", "style_id": 2, "points": points}],
        )
    else:
        test.mutate(
            "raster.patch",
            document,
            **target,
            coordinate_space="raster_pixels_lower_left",
            x=0,
            y=0,
            width=2,
            height=2,
            pixels=[{"ink": 3, "paint": 2, "tone": tone} for tone in (0, 64, 128, 255)],
        )


def _artwork(
    test: NativeAcceptance,
    document: str,
    level_id: str,
    image_format: str,
) -> dict[str, Any]:
    target = {"document_id": document, "level_id": level_id, "drawing_id": DRAWING}
    if image_format == "vector":
        geometry = test.success("vector.inspect", target)
        return {
            "total": geometry["total"],
            "strokes": [
                {key: stroke[key] for key in ("style_id", "closed", "points")}
                for stroke in geometry["strokes"]
            ],
        }
    return test.success(
        "raster.read_pixels",
        {
            **target,
            "coordinate_space": "raster_pixels_lower_left",
            "x": 0,
            "y": 0,
            "width": 2,
            "height": 2,
        },
    )


def _edit_pages(test: NativeAcceptance, document: str, level_id: str) -> int:
    for name in ("紙 Ω", "Highlights"):
        test.mutate("palette.page.create", document, level_id=level_id, name=name)
    test.mutate("palette.page.rename", document, level_id=level_id, page_index=0, name="Base")
    for style_id in (2, 3, 2):
        test.mutate(
            "palette.style.move",
            document,
            level_id=level_id,
            style_id=style_id,
            page_index=1,
            position=0,
        )
    cloned = test.mutate(
        "palette.clone_style",
        document,
        level_id=level_id,
        style_id=2,
        page_index=2,
        name="Copy Ω",
    )
    test.assertEqual(cloned["style_id"], 4)
    source = _styles(test, document, level_id)[2]
    test.assertEqual(cloned["animation"], source["animation"])
    test.mutate(
        "palette.rename_style",
        document,
        level_id=level_id,
        style_id=2,
        name="元 Ω",
    )
    test.mutate("palette.page.move", document, level_id=level_id, page_index=2, to_index=1)
    return cloned["style_id"]


def _assert_noops(test: NativeAcceptance, document: str, level_id: str) -> None:
    revision = test.revisions[document]
    for method, fields in (
        ("palette.page.rename", {"page_index": 0, "name": "Base"}),
        ("palette.page.move", {"page_index": 1, "to_index": 1}),
        ("palette.style.move", {"style_id": 2, "page_index": 2, "position": 0}),
        ("palette.rename_style", {"style_id": 2, "name": "元 Ω"}),
    ):
        test.mutate(method, document, level_id=level_id, **fields)
        test.assertEqual(test.revisions[document], revision, method)


def _assert_lifecycle(test: NativeAcceptance, image_format: str) -> None:
    document, level_id = _fixture(test, image_format)
    artwork = _artwork(test, document, level_id, image_format)
    cloned_id = _edit_pages(test, document, level_id)
    source = _styles(test, document, level_id)[2]
    test.mutate(
        "palette.update_style",
        document,
        level_id=level_id,
        style_id=cloned_id,
        keyframe=12,
        color={"r": 10, "g": 240, "b": 90, "a": 255},
    )
    test.assertEqual(_styles(test, document, level_id)[2], source)
    pages = _pages(test, document, level_id)
    test.assertEqual([page["name"] for page in pages["pages"]], ["Base", "Highlights", "紙 Ω"])
    test.assertEqual([page["style_ids"] for page in pages["pages"]], [[0, 1], [4], [2, 3]])
    test.mutate("palette.page.create", document, level_id=level_id, name="Temporary")
    test.mutate("palette.page.remove", document, level_id=level_id, page_index=3)
    test.assertEqual(_pages(test, document, level_id), pages)
    test.assertEqual(_artwork(test, document, level_id, image_format), artwork)
    _assert_noops(test, document, level_id)
    expected_styles = _styles(test, document, level_id)
    saved = test.mutate("scene.save_copy", document, directory=str(test.workspace / "palette-copy"))
    opened = test.mutate("scene.open", None, path=saved["package_directory"])
    restored = opened["document_id"]
    summary = test.success("scene.summary", {"document_id": restored})
    restored_level = summary["levels"][0]["level_id"]
    test.assertEqual(_pages(test, restored, restored_level), pages)
    test.assertEqual(_styles(test, restored, restored_level), expected_styles)
    test.assertEqual(_artwork(test, restored, restored_level, image_format), artwork)
    cells = test.success(
        "column.cells",
        {
            "document_id": restored,
            "column_id": summary["columns"][0]["column_id"],
            "start_frame": 0,
            "length": 2,
        },
    )
    test.assertEqual([cell["drawing_id"] for cell in cells["cells"]], [DRAWING, DRAWING])


def check_vector_palette_lifecycle(test: NativeAcceptance) -> None:
    """Preserve vector style IDs, page order, and independent animated copies.

    Args:
        test: The running native acceptance case and disposable workspace.
    """
    _assert_lifecycle(test, "vector")


def check_toonz_palette_lifecycle(test: NativeAcceptance) -> None:
    """Preserve TLV ink/paint references and ordered animated palette pages.

    Args:
        test: The running native acceptance case and disposable workspace.
    """
    _assert_lifecycle(test, "toonzRaster")


def check_palette_page_rejections(test: NativeAcceptance) -> None:
    """Reject unsafe page operations without changing artwork, keys, or revision.

    Args:
        test: The running native acceptance case and disposable workspace.
    """
    document, level_id = _fixture(test, "vector")
    test.mutate("palette.page.create", document, level_id=level_id, name="Destination")
    before = _pages(test, document, level_id), _styles(test, document, level_id)
    revision = test.revisions[document]
    artwork = _artwork(test, document, level_id, "vector")
    for method, fields in (
        ("palette.page.rename", {"page_index": 0, "name": "refimage-bad"}),
        ("palette.page.rename", {"page_index": 0, "name": "NUL\0name"}),
        ("palette.page.create", {"name": ""}),
        ("palette.page.create", {"name": "x" * 129}),
        ("palette.page.remove", {"page_index": 0}),
        ("palette.page.move", {"page_index": 1, "to_index": 0}),
        ("palette.page.rename", {"page_index": 2, "name": "Missing"}),
        ("palette.style.move", {"style_id": 0, "page_index": 1, "position": 0}),
        ("palette.style.move", {"style_id": 2, "page_index": 0, "position": 0}),
        ("palette.style.move", {"style_id": 2, "page_index": 1, "position": 1}),
        ("palette.rename_style", {"style_id": 2, "name": "|linked"}),
        ("palette.clone_style", {"style_id": 2, "page_index": 2, "name": "Missing"}),
    ):
        test.failure(method, test.write_params(document, {"level_id": level_id, **fields}))
        test.assertEqual(
            (_pages(test, document, level_id), _styles(test, document, level_id)), before
        )
        test.assertEqual(
            test.success("scene.summary", {"document_id": document})["revision"], revision
        )
    test.assertEqual(_artwork(test, document, level_id, "vector"), artwork)
    test.mutate(
        "palette.style.move",
        document,
        level_id=level_id,
        style_id=2,
        page_index=1,
        position=0,
    )
    before = _pages(test, document, level_id), _styles(test, document, level_id)
    revision = test.revisions[document]
    error = test.failure(
        "palette.page.remove",
        test.write_params(document, {"level_id": level_id, "page_index": 1}),
    )
    test.assertEqual(error["code"], "page_not_empty")
    test.assertEqual((_pages(test, document, level_id), _styles(test, document, level_id)), before)
    test.assertEqual(test.success("scene.summary", {"document_id": document})["revision"], revision)


def check_palette_page_capacity(test: NativeAcceptance) -> None:
    """Accept 128 pages, reject page 129, and reclaim a removed empty page.

    Args:
        test: The running native acceptance case and disposable workspace.
    """
    document, level_id = _fixture(test, "vector")
    for index in range(1, 128):
        test.mutate("palette.page.create", document, level_id=level_id, name=f"Page {index}")
    pages = _pages(test, document, level_id)
    revision = test.revisions[document]
    test.assertEqual(pages["page_count"], 128)
    error = test.failure(
        "palette.page.create",
        test.write_params(document, {"level_id": level_id, "name": "Overflow"}),
    )
    test.assertEqual(error["code"], "resource_limit")
    test.assertEqual(_pages(test, document, level_id), pages)
    test.assertEqual(test.success("scene.summary", {"document_id": document})["revision"], revision)
    test.mutate("palette.page.remove", document, level_id=level_id, page_index=127)
    test.mutate("palette.page.create", document, level_id=level_id, name="Reclaimed")
    pages = _pages(test, document, level_id)
    test.assertEqual(pages["page_count"], 128)
    saved = test.mutate(
        "scene.save_copy", document, directory=str(test.workspace / "palette-capacity")
    )
    opened = test.mutate("scene.open", None, path=saved["package_directory"])
    restored = opened["document_id"]
    summary = test.success("scene.summary", {"document_id": restored})
    test.assertEqual(_pages(test, restored, summary["levels"][0]["level_id"]), pages)
