# Review fills before accepting a reshape

Enable **Review Fills** in the Control Point Editor or vector Selection tool.
After an edit, the native review shows immutable original and proposed drawings,
including their actual fills and filled-region counts. Multiple selected drawings
appear in the drawing selector. Inspect every drawing before choosing **Apply**.
**Restore Original**, Escape, or closing the dialog restores the original stroke
geometry and fill assignments without adding undo history or marking the scene
changed. Switching the scene, drawing, column, level or tool rejects the review.

The Control Point Editor covers control-point, tangent and segment drags, point
insertion, tangent linking/linearization, and arrow nudges. The Selection tool
covers Free Deform and Envelope, including Whole Level and selected-frame modes;
the same transaction also reviews move, rotation and scale operations. Escape
or deactivating the tool during an unfinished vector drag restores its original
geometry. Spline paths do not use the vector fill dialog.

Apply records one undo transaction for the operation. Undo and redo restore both
the corresponding geometry and saved fill assignments. This does not invent a
fill for newly closed regions: the proposed preview shows the actual topology
and colors that will be accepted, including lost or newly unfilled regions.

Native UI checks in `toonz/sources/toonz/fillworkflowintegrationtest.cpp` drive
Control Point Editor cancellation, Apply, undo/redo and Escape, then repeat the
review/apply/recovery path across two drawings for Free Deform and Envelope.
They also switch tools during both single-drawing and multi-drawing reviews,
and assert that reviewing and cancelling do not dirty the scene or add history.
The native host fixture registers these checks; execution is pending CI.
