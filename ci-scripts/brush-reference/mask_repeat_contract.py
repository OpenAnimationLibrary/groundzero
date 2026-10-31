"""Fixed diagnostic controls; never a replacement for the maskwash matrix."""
GROUPS = {
    "maskrepeat-reused": (False, None),
    "maskrepeat-freshnode": (True, None),
    "maskrepeat-single-reused": (False, 1),
    "maskrepeat-single-freshnode": (True, 1),
}
INDICES = (43, 44, 70, 71)
REPETITIONS = 8


def contract(group):
    if group not in GROUPS:
        return None
    fresh, workers = GROUPS[group]
    return {"version": 1, "sourceGroup": "maskwash", "indices": list(INDICES),
            "repetitions": REPETITIONS, "nodePolicy": "fresh" if fresh else "reused",
            "configuredMaxThreads": workers, "sourceParityQualified": False}


def fresh_node(document, node):
    """Public Node API only; wait before changing the active paint target."""
    document.waitForDone()
    replacement = document.createNode("Original repeat diagnostic", "paintlayer")
    if not replacement or not document.rootNode().addChildNode(replacement, None):
        raise RuntimeError("Cannot create diagnostic source paint node")
    document.setActiveNode(replacement)
    if document.activeNode() != replacement:
        raise RuntimeError("Diagnostic paint node did not become active")
    if not node.remove():
        raise RuntimeError("Cannot remove previous diagnostic source paint node")
    document.waitForDone()
    return replacement
