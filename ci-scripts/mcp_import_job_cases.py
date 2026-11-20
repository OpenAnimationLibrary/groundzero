"""Qualify the editable import job: consent, the source copy, adoption, cancel and the corpus.

`scene.import_editable` converts a read-only document in its scene host into a verified
package under the host's private directory; `scene.open {import_job_id}` adopts it; every
later `scene.save_copy` carries the provenance and the original scene file. The user's files
never change, a job that does not complete leaves nothing, and a retry with the same
operation_id returns the same job. The corpus case imports every old scene, saves it as a
package, reopens it and renders it: exactly the baselines where the plan has no loss, and
exactly a copy of the scene with the lost item removed where it has one.
"""

from __future__ import annotations

import base64
import json
import re
import shutil
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from mcp_case import acceptance_case
from mcp_corpus_compare import CORPUS, compare_scene, load_manifest
from mcp_failure_cases import _fault_worker
from mcp_import_cases import plan
from mcp_native_oracle import compare_files
from mcp_scene_access_cases import TERMINAL, _op, corpus, discard, open_scene, project_id
from mcp_studio_palette_cases import call, refuse, tree_digest

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance
    from mcp_worker import Worker

FAULT_FLAG = "needs --test-write-fault, which the adapter never passes"


def start(
    case: NativeAcceptance,
    worker: Worker,
    document: str,
    digest: str,
    accept: list[str],
    operation: str = "",
) -> dict[str, Any]:
    params = {
        "document_id": document,
        "expected_revision": "0",
        "plan_digest": digest,
        "accept_losses": accept,
        "operation_id": operation or _op(case),
    }
    return worker.call("scene.import_editable", params)


def finish(case: NativeAcceptance, worker: Worker, job: str) -> dict[str, Any]:
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        status = call(case, worker, "job.status", {"job_id": job})
        if status["status"] in TERMINAL:
            return status
        time.sleep(0.05)
    case.fail(f"Import job {job} did not finish")


def imported(case: NativeAcceptance, worker: Worker, document: str) -> dict[str, Any]:
    """Plan, import with every loss kind accepted, wait, and return the completed record."""
    result = plan(case, worker, document)
    kinds = [row["kind"] for row in result["losses"]]
    response = start(case, worker, document, result["plan_digest"], kinds)
    case.assertNotIn("error", response, response)
    record = finish(case, worker, response["result"]["job_id"])
    case.assertEqual(record["status"], "completed", record)
    applied = {row["kind"] for row in record["import"]["losses_applied"]}
    case.assertEqual(applied, set(kinds))
    return record


def adopt(case: NativeAcceptance, worker: Worker, job: str) -> dict[str, Any]:
    params = {"import_job_id": job, "operation_id": _op(case)}
    return call(case, worker, "scene.open", params)


def save(case: NativeAcceptance, worker: Worker, document: str, revision: str, name: str) -> Path:
    target = case.workspace / name
    params = {
        "document_id": document,
        "expected_revision": revision,
        "directory": str(target),
        "operation_id": _op(case),
    }
    call(case, worker, "scene.save_copy", params)
    return target


def _refusals(case: NativeAcceptance, worker: Worker, document: str, result: dict) -> None:
    """Without every loss kind, or with another digest, nothing starts."""
    kinds = sorted(row["kind"] for row in result["losses"])
    response = start(case, worker, document, result["plan_digest"], kinds[1:])
    case.assertEqual(response["error"]["code"], "losses_not_accepted", response)
    case.assertEqual(response["error"]["details"]["missing"], kinds[:1])
    response = start(case, worker, document, "plan:" + "0" * 64, kinds)
    case.assertEqual(response["error"]["code"], "plan_changed", response)
    case.assertEqual(response["error"]["details"]["plan_digest"], result["plan_digest"])
    response = start(case, worker, document, result["plan_digest"], [*kinds, kinds[0]])
    case.assertIn("error", response, response)
    listed = call(case, worker, "job.list", {"kind": "scene_import"})
    case.assertEqual(listed["total"], 0, listed)


def _check_adopted(case: NativeAcceptance, opened: dict[str, Any], digest: str) -> None:
    case.assertEqual(opened["admission"], "import_job")
    case.assertIsNone(opened["location"])
    provenance = opened["provenance"]
    case.assertEqual(provenance["kind"], "import")
    case.assertEqual(provenance["plan_digest"], digest)
    case.assertEqual(provenance["source_copy"], "provenance/source-scene.tnz")
    source = provenance["source"]
    case.assertEqual(
        (source["root"], source["relative_path"]), ("input_root", "corpus/Unsafe/shot_c.tnz")
    )
    case.assertEqual(source["unrecognized_scene_tags"], ["gzFutureData"])
    case.assertNotIn(str(case.inputs), json.dumps(provenance))


def _check_package(case: NativeAcceptance, package: Path, original: bytes, digest: str) -> None:
    manifest = json.loads((package / "manifest.json").read_text())
    case.assertEqual(manifest["provenance"]["plan_digest"], digest)
    copy = package / "provenance" / "source-scene.tnz"
    case.assertEqual(copy.read_bytes(), original, "the source copy is not the original")
    listed = {row["path"] for row in manifest["files"]}
    case.assertIn("provenance/source-scene.tnz", listed)


@acceptance_case
def unknown_tags_import_with_source_copy(case: NativeAcceptance) -> None:
    """Consent is per kind; the adopted document and every later save carry the original."""
    scenes = corpus(case)
    worker = case.worker
    original = scenes["C"].read_bytes()
    before = tree_digest(case.inputs)
    document = open_scene(case, worker, scenes["C"], "standalone")["document_id"]
    result = plan(case, worker, document)
    _refusals(case, worker, document, result)
    operation = _op(case)
    kinds = [row["kind"] for row in result["losses"]]
    first = start(case, worker, document, result["plan_digest"], kinds, operation)["result"]
    case.assertEqual((first["kind"], first["lane"]), ("scene_import", "render"))
    again = start(case, worker, document, result["plan_digest"], kinds, operation)["result"]
    case.assertEqual(again["job_id"], first["job_id"])
    record = finish(case, worker, first["job_id"])
    case.assertEqual(record["status"], "completed", record)
    case.assertRegex(record["import"]["package_id"], r"^package:[0-9a-f]{64}$")
    case.assertTrue(record["import"]["source_copy"])
    listed = call(case, worker, "job.list", {"kind": "scene_import"})
    case.assertEqual(listed["total"], 1, listed)
    opened = adopt(case, worker, first["job_id"])
    _check_adopted(case, opened, result["plan_digest"])
    status = call(case, worker, "job.status", {"job_id": first["job_id"]})
    case.assertEqual(status["import"]["adopted_documents"], 1)
    cast = call(case, worker, "cast.list", {**_doc(opened), "limit": 64})["items"]
    case.assertEqual(sorted(row["name"] for row in cast), ["Ink", "Paint"])
    first_save = save(case, worker, opened["document_id"], opened["revision"], "imported")
    _check_package(case, first_save, original, result["plan_digest"])
    reopened = call(
        case, worker, "scene.open", {"path": str(first_save), "operation_id": _op(case)}
    )
    case.assertEqual(reopened["provenance"]["plan_digest"], result["plan_digest"])
    resaved = save(case, worker, reopened["document_id"], reopened["revision"], "resaved")
    _check_package(case, resaved, original, result["plan_digest"])
    both = {"path": str(first_save), "import_job_id": first["job_id"], "operation_id": _op(case)}
    refuse(case, worker, "scene.open", both, "invalid_params")
    unknown = {"import_job_id": "job_00000000_x", "operation_id": _op(case)}
    refuse(case, worker, "scene.open", unknown, "unknown_job")
    discard(case, worker, document)
    case.assertEqual(tree_digest(case.inputs), before)


def _doc(opened: dict[str, Any]) -> dict[str, Any]:
    return {"document_id": opened["document_id"], "expected_revision": opened["revision"]}


@acceptance_case
def structured_scene_imports_its_accepted_losses(case: NativeAcceptance) -> None:
    """The sub-xsheet goes, the 16-bit TIFF becomes 8-bit and the blur is clamped."""
    scenes = corpus(case)
    worker = case.worker
    before = tree_digest(case.inputs)
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    record = imported(case, worker, document)
    opened = adopt(case, worker, record["job_id"])
    summary = call(case, worker, "scene.summary", {"document_id": opened["document_id"]})
    levels = {row["name"] for row in summary["levels"]}
    case.assertNotIn("Sub", levels)
    case.assertIn("Deep", levels)
    package = save(case, worker, opened["document_id"], opened["revision"], "structured")
    manifest = json.loads((package / "manifest.json").read_text())
    deep = next(row for row in manifest["levels"] if row["name"] == "Deep")
    frame = package / deep["path"].replace("..png", ".0001.png")
    case.assertEqual(frame.read_bytes()[24], 8, "the 16-bit TIFF was not reduced to 8 bits")
    source = (package / "provenance" / "source-scene.tnz").read_bytes()
    case.assertEqual(source, scenes["B"].read_bytes(), "a scene without unknown tags is attached")
    issues = call(case, worker, "scene.validate", _doc(opened))
    severities = {row["severity"] for row in issues["items"]}
    case.assertLessEqual(severities, {"info"}, issues)
    second = adopt(case, worker, record["job_id"])
    case.assertNotEqual(second["document_id"], opened["document_id"])
    status = call(case, worker, "job.status", {"job_id": record["job_id"]})
    case.assertEqual(status["import"]["adopted_documents"], 2)
    discard(case, worker, document)
    case.assertEqual(tree_digest(case.inputs), before)


def _host_imports(case: NativeAcceptance, job: str) -> list[Path]:
    return list(case.workspace.glob(f"runtime_*/scene-host-*/imports/{job}"))


@acceptance_case(worker_only=FAULT_FLAG)
def source_changed_during_import_adopts_nothing(case: NativeAcceptance) -> None:
    """A source changed while the job runs fails it; a canceled job ends the same way: in
    both, nothing is left in the host and nothing can be adopted."""
    _fault_worker(case, "import-hold", "--test-write-fault", "import:written:hold")
    scenes = corpus(case)
    worker = case.worker
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    result = plan(case, worker, document)
    kinds = [row["kind"] for row in result["losses"]]
    job = start(case, worker, document, result["plan_digest"], kinds)["result"]["job_id"]
    time.sleep(3)
    tiff = scenes["B"].parent / "assets" / "deep.0001.tif"
    tiff.write_bytes(tiff.read_bytes()[:-4] + b"\x11\x11\x11\x11")
    record = finish(case, worker, job)
    case.assertEqual(record["status"], "failed", record)
    case.assertEqual(record["failure"]["code"], "source_changed", record)
    case.assertIsNone(record["import"]["package_id"])
    details = refuse(
        case,
        worker,
        "scene.open",
        {"import_job_id": job, "operation_id": _op(case)},
        "import_not_ready",
    )
    case.assertEqual(details["status"], "failed")
    case.assertEqual(_host_imports(case, job), [])
    discard(case, worker, document)
    document = open_scene(case, worker, scenes["B"], "standalone")["document_id"]
    result = plan(case, worker, document)
    job = start(case, worker, document, result["plan_digest"], kinds)["result"]["job_id"]
    time.sleep(3)
    call(case, worker, "job.cancel", {"job_id": job, "operation_id": _op(case)})
    record = finish(case, worker, job)
    case.assertEqual(record["status"], "canceled", record)
    case.assertEqual(_host_imports(case, job), [])
    discard(case, worker, document)


# ------------------------------------------------------------------- corpus


def _install(case: NativeAcceptance, name: str) -> Path:
    target = case.inputs / "corpus" / name
    shutil.copytree(CORPUS / name, target)
    return target / "scenes" / "shot.tnz"


def _render(case: NativeAcceptance, worker: Worker, read: dict[str, Any], folder: Path) -> list:
    frames = load_manifest()["frames"]
    request = {
        **read,
        "start_frame": frames["first"],
        "end_frame": frames["last"],
        "step": 1,
        "width": frames["width"],
        "height": frames["height"],
        "operation_id": _op(case),
    }
    job = call(case, worker, "render.submit", request)["job_id"]
    case.assertEqual(finish(case, worker, job)["status"], "completed")
    artifacts = call(case, worker, "job.result", {"job_id": job})["artifacts"]
    folder.mkdir(parents=True)
    written = []
    for artifact in sorted(artifacts, key=lambda item: item["frame"]):
        image = call(case, worker, "artifact.read", {"artifact_id": artifact["artifact_id"]})
        path = folder / f"frame.{artifact['frame'] + 1:04d}.png"
        path.write_bytes(base64.b64decode(image["image"]["data"], validate=True))
        written.append(path)
    return written


def _without_sub_xsheets(scene: Path) -> Path:
    """The scene with every top-sheet cell of a sub-xsheet removed, as the import does."""
    text = scene.read_text()
    text = re.sub(r"<cell>\s*\d+ \d+ <childLevel id='\d+'/>[^<]*</cell>", "", text)
    variant = scene.with_name("shot_without_sub_xsheets.tnz")
    variant.write_text(text)
    return variant


def _expected(case: NativeAcceptance, scene: Path, name: str, kinds: set[str]) -> list[Path]:
    """The frames a read-only render of the scene with the lost items removed gives."""
    # A dropped level the scene never exposes needs no removal from the copy:
    # the equal frames then show that it did not contribute.
    expected = {"sub_xsheet", "level_not_admitted"}
    case.assertLessEqual(kinds, expected, f"{name}: no expectation for these losses")
    worker = case.worker
    variant = open_scene(case, worker, _without_sub_xsheets(scene), project_id(case, worker, name))
    frames = _render(case, worker, _doc(variant), case.directory / "expected" / name)
    discard(case, worker, variant["document_id"])
    return frames


def _import_scene(case: NativeAcceptance, name: str) -> tuple[set[str], list[Path], Path]:
    worker = case.worker
    scene = _install(case, name)
    opened = open_scene(case, worker, scene, project_id(case, worker, name))
    kinds = {row["kind"] for row in plan(case, worker, opened["document_id"])["losses"]}
    record = imported(case, worker, opened["document_id"])
    adopted = adopt(case, worker, record["job_id"])
    package = save(case, worker, adopted["document_id"], adopted["revision"], f"pkg-{name}")
    reopened = call(case, worker, "scene.open", {"path": str(package), "operation_id": _op(case)})
    frames = _render(case, worker, _doc(reopened), case.directory / "imported" / name)
    discard(case, worker, opened["document_id"])
    return kinds, frames, scene


@acceptance_case
def corpus_scenes_import_save_reopen_and_render_the_same(case: NativeAcceptance) -> None:
    """Each corpus scene imports, saves as a package, reopens and renders its baselines
    exactly when the plan reports no loss, and exactly the scene without the lost items
    otherwise; the corpus never changes."""
    manifest = load_manifest()
    for name in manifest["scenes"]:
        with case.subTest(scene=name):
            kinds, frames, scene = _import_scene(case, name)
            case.assertEqual(tree_digest(scene.parent.parent), tree_digest(CORPUS / name))
            if not kinds:
                case.assertEqual(compare_scene(manifest, name, frames), [], name)
                continue
            expected = _expected(case, scene, name, kinds)
            for wanted, got in zip(expected, frames, strict=True):
                case.assertEqual(compare_files(wanted, got).problems, [], f"{name} {got.name}")
