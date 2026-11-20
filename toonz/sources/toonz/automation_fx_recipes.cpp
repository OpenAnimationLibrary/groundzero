#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include <QJsonDocument>

namespace GroundzeroAutomation {
namespace {

// Static recipe documents, one per entry so that no string literal outgrows a
// compiler's limit. Steps are atomic worker tool calls. An argument whose whole
// value is "${input.<name>}" or "${steps.<id>.<path>}" takes that input's or
// earlier result's JSON value; a path continues with dotted field names and
// array indexes. A step with "poll" is repeated until its field is terminal.
// Check frames use the input defaults.
const char* const recipeTexts[] = {
    R"json({
 "id": "light-wrap-composite",
 "version": 2,
 "title": "Light wrap composite",
 "outcome": "Background light spills over the foreground edges.",
 "category": "light",
 "brief": "Adds a light wrap effect: the background's light spills over the foreground's edges. The foreground and background effects already exist; the recipe only adds the wrap and rewires the current output to it.",
 "required_types": [
  "STD_lightWrapFx"
 ],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "fx.connect",
   "fx.create",
   "fx.graph",
   "fx.parameter.set"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "inspect"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "foreground_fx_id and background_fx_id name effects in that document."
 ],
 "ownership": {
  "creates": [
   "wrap"
  ],
  "edits": [
   "document_id"
  ],
  "leaves_alone": "The two input effects keep their own parameters; only the current output is reconnected to the wrap."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "foreground_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "background_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  }
 ],
 "steps": [
  {
   "id": "graph",
   "tool": "fx.graph",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "wrap",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "STD_lightWrapFx",
    "name": "Light wrap"
   }
  },
  {
   "id": "source",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.wrap.fx_id}",
    "port": "Source",
    "source_fx_id": "${input.foreground_fx_id}"
   }
  },
  {
   "id": "background",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.wrap.fx_id}",
    "port": "Background",
    "source_fx_id": "${input.background_fx_id}"
   }
  },
  {
   "id": "width",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.wrap.fx_id}",
    "parameter": "width",
    "value": 12
   }
  },
  {
   "id": "output",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}",
    "port": "source",
    "source_fx_id": "${steps.wrap.fx_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "wrap_width",
   "step": "wrap",
   "parameter": "width"
  },
  {
   "name": "softness",
   "step": "wrap",
   "parameter": "softness"
  },
  {
   "name": "intensity",
   "step": "wrap",
   "parameter": "intensity"
  }
 ],
 "seeds": [],
 "checks": [
  {
   "kind": "diagnostics_clean"
  },
  {
   "kind": "render_alpha_nonempty",
   "frames": [
    0
   ]
  },
  {
   "kind": "reopen_pixels_equal",
   "frames": [
    0
   ]
  }
 ],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; the check frame is 0",
  "lengths": "not used",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "effect",
   "label": "editable",
   "from": "steps.wrap.fx_id",
   "note": "The light wrap effect, a native effect the client can keep editing."
  }
 ],
 "review": {
  "kind": "checks"
 },
 "limits": []
})json",
    R"json({
 "id": "backlit-rim-light",
 "version": 2,
 "title": "Backlit rim light",
 "outcome": "A light source glows through the subject's silhouette edges.",
 "category": "light",
 "brief": "Adds a backlit effect: a light source glows through the subject's silhouette edges. The light and subject effects already exist; the recipe adds the rim effect and rewires the current output to it.",
 "required_types": [
  "STD_backlitFx"
 ],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "fx.connect",
   "fx.create",
   "fx.graph",
   "fx.parameter.set"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "inspect"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "light_fx_id and subject_fx_id name effects in that document."
 ],
 "ownership": {
  "creates": [
   "rim"
  ],
  "edits": [
   "document_id"
  ],
  "leaves_alone": "The light and subject effects keep their own parameters; only the current output is reconnected to the rim effect."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "light_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "subject_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  }
 ],
 "steps": [
  {
   "id": "graph",
   "tool": "fx.graph",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "rim",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "STD_backlitFx",
    "name": "Rim light"
   }
  },
  {
   "id": "light",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.rim.fx_id}",
    "port": "Light",
    "source_fx_id": "${input.light_fx_id}"
   }
  },
  {
   "id": "subject",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.rim.fx_id}",
    "port": "Source",
    "source_fx_id": "${input.subject_fx_id}"
   }
  },
  {
   "id": "width",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.rim.fx_id}",
    "parameter": "value",
    "value": 8
   }
  },
  {
   "id": "output",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}",
    "port": "source",
    "source_fx_id": "${steps.rim.fx_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "rim_width",
   "step": "rim",
   "parameter": "value"
  },
  {
   "name": "rim_color",
   "step": "rim",
   "parameter": "color"
  },
  {
   "name": "fade",
   "step": "rim",
   "parameter": "fade"
  }
 ],
 "seeds": [],
 "checks": [
  {
   "kind": "diagnostics_clean"
  },
  {
   "kind": "render_alpha_nonempty",
   "frames": [
    0
   ]
  },
  {
   "kind": "reopen_pixels_equal",
   "frames": [
    0
   ]
  }
 ],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; the check frame is 0",
  "lengths": "not used",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "effect",
   "label": "editable",
   "from": "steps.rim.fx_id",
   "note": "The rim light effect, a native effect the client can keep editing."
  }
 ],
 "review": {
  "kind": "checks"
 },
 "limits": []
})json",
    R"json({
 "id": "exposure-grade",
 "version": 2,
 "title": "Exposure grade",
 "outcome": "The source is re-exposed in stops, then its gamma is adjusted.",
 "category": "light",
 "brief": "Re-exposes the source in stops with an exposure effect, then adjusts its gamma with a second effect. The source effect already exists; the recipe adds two effects and rewires the current output to the second.",
 "required_types": [
  "STD_iwa_AdjustExposureFx",
  "STD_gammaFx"
 ],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "fx.connect",
   "fx.create",
   "fx.graph",
   "fx.parameter.set"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "inspect"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "source_fx_id names an effect in that document."
 ],
 "ownership": {
  "creates": [
   "exposure",
   "gamma"
  ],
  "edits": [
   "document_id"
  ],
  "leaves_alone": "The source effect keeps its own parameters; only the current output is reconnected to the gamma effect."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "source_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  }
 ],
 "steps": [
  {
   "id": "graph",
   "tool": "fx.graph",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "exposure",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "STD_iwa_AdjustExposureFx",
    "name": "Exposure"
   }
  },
  {
   "id": "source",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.exposure.fx_id}",
    "port": "Source",
    "source_fx_id": "${input.source_fx_id}"
   }
  },
  {
   "id": "stops",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.exposure.fx_id}",
    "parameter": "scale",
    "value": 0.5
   }
  },
  {
   "id": "gamma",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "STD_gammaFx",
    "name": "Grade gamma"
   }
  },
  {
   "id": "graded",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.gamma.fx_id}",
    "port": "Source",
    "source_fx_id": "${steps.exposure.fx_id}"
   }
  },
  {
   "id": "curve",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.gamma.fx_id}",
    "parameter": "value",
    "value": 1.4
   }
  },
  {
   "id": "output",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}",
    "port": "source",
    "source_fx_id": "${steps.gamma.fx_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "exposure",
   "step": "exposure",
   "parameter": "scale"
  },
  {
   "name": "offset",
   "step": "exposure",
   "parameter": "offset"
  },
  {
   "name": "gamma",
   "step": "gamma",
   "parameter": "value"
  }
 ],
 "seeds": [],
 "checks": [
  {
   "kind": "diagnostics_clean"
  },
  {
   "kind": "render_alpha_nonempty",
   "frames": [
    0
   ]
  },
  {
   "kind": "reopen_pixels_equal",
   "frames": [
    0
   ]
  }
 ],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; the check frame is 0",
  "lengths": "not used",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "effect",
   "label": "editable",
   "from": "steps.gamma.fx_id",
   "note": "The last effect of the grade chain, a native effect the client can keep editing."
  }
 ],
 "review": {
  "kind": "checks"
 },
 "limits": []
})json",
    R"json({
 "id": "cross-dissolve-transition",
 "version": 2,
 "title": "Cross dissolve",
 "outcome": "The from source dissolves into the to source between two frames.",
 "category": "transition",
 "brief": "Dissolves the from source into the to source between start_frame and end_frame with a blend effect keyed from 0 to 100. Both sources already exist; the recipe adds the blend and rewires the current output to it.",
 "required_types": [
  "blendFx"
 ],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "fx.connect",
   "fx.create",
   "fx.graph",
   "fx.parameter.set_key"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "inspect"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "from_fx_id and to_fx_id name effects in that document.",
  "start_frame is before end_frame."
 ],
 "ownership": {
  "creates": [
   "dissolve"
  ],
  "edits": [
   "document_id"
  ],
  "leaves_alone": "The two sources keep their own parameters; only the current output is reconnected to the blend."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "from_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "to_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "start_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 0
   }
  },
  {
   "name": "end_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 12
   }
  }
 ],
 "steps": [
  {
   "id": "graph",
   "tool": "fx.graph",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "dissolve",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "blendFx",
    "name": "Cross dissolve"
   }
  },
  {
   "id": "to",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}",
    "port": "Source1",
    "source_fx_id": "${input.to_fx_id}"
   }
  },
  {
   "id": "from",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}",
    "port": "Source2",
    "source_fx_id": "${input.from_fx_id}"
   }
  },
  {
   "id": "start",
   "tool": "fx.parameter.set_key",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}",
    "parameter": "value",
    "frame": "${input.start_frame}",
    "value": 0
   }
  },
  {
   "id": "end",
   "tool": "fx.parameter.set_key",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.dissolve.fx_id}",
    "parameter": "value",
    "frame": "${input.end_frame}",
    "value": 100
   }
  },
  {
   "id": "output",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}",
    "port": "source",
    "source_fx_id": "${steps.dissolve.fx_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "mix",
   "step": "dissolve",
   "parameter": "value"
  }
 ],
 "seeds": [],
 "checks": [
  {
   "kind": "diagnostics_clean"
  },
  {
   "kind": "frames_differ",
   "frames": [
    0,
    6,
    12
   ]
  },
  {
   "kind": "reopen_pixels_equal",
   "frames": [
    0,
    6,
    12
   ]
  }
 ],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; start_frame and end_frame are frames of the scene's own timeline",
  "lengths": "not used",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "effect",
   "label": "editable",
   "from": "steps.dissolve.fx_id",
   "note": "The blend effect with its two keys, a native effect the client can keep editing."
  }
 ],
 "review": {
  "kind": "checks"
 },
 "limits": []
})json",
    R"json({
 "id": "iris-wipe-transition",
 "version": 2,
 "title": "Iris wipe",
 "outcome": "A soft circular matte opens from the center to reveal the subject.",
 "category": "transition",
 "brief": "Opens a soft circular matte from the centre to reveal the subject between start_frame and end_frame. The subject effect already exists; the recipe adds a keyed radial gradient and a matte effect and rewires the current output to the matte.",
 "required_types": [
  "STD_radialGradientFx",
  "inFx"
 ],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "fx.connect",
   "fx.create",
   "fx.graph",
   "fx.parameter.set",
   "fx.parameter.set_key"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "inspect"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "subject_fx_id names an effect in that document.",
  "start_frame is before end_frame and end_frame is below frame_count."
 ],
 "ownership": {
  "creates": [
   "iris",
   "matte"
  ],
  "edits": [
   "document_id"
  ],
  "leaves_alone": "The subject effect keeps its own parameters; only the current output is reconnected to the matte."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "subject_fx_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "start_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 0
   }
  },
  {
   "name": "end_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 12
   }
  },
  {
   "name": "frame_count",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 10000,
    "default": 13
   }
  },
  {
   "name": "open_radius",
   "schema": {
    "type": "number",
    "minimum": 0.01,
    "maximum": 100000,
    "default": 20
   }
  }
 ],
 "steps": [
  {
   "id": "graph",
   "tool": "fx.graph",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "iris",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "STD_radialGradientFx",
    "name": "Iris",
    "frame_count": "${input.frame_count}"
   }
  },
  {
   "id": "inside",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.iris.fx_id}",
    "parameter": "color1",
    "value": [
     255,
     255,
     255,
     255
    ]
   }
  },
  {
   "id": "outside",
   "tool": "fx.parameter.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.iris.fx_id}",
    "parameter": "color2",
    "value": [
     0,
     0,
     0,
     0
    ]
   }
  },
  {
   "id": "closed",
   "tool": "fx.parameter.set_key",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.iris.fx_id}",
    "parameter": "period",
    "frame": "${input.start_frame}",
    "value": 0.01
   }
  },
  {
   "id": "open",
   "tool": "fx.parameter.set_key",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.iris.fx_id}",
    "parameter": "period",
    "frame": "${input.end_frame}",
    "value": "${input.open_radius}"
   }
  },
  {
   "id": "matte",
   "tool": "fx.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "type": "inFx",
    "name": "Iris matte"
   }
  },
  {
   "id": "subject",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.matte.fx_id}",
    "port": "Source",
    "source_fx_id": "${input.subject_fx_id}"
   }
  },
  {
   "id": "shape",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.matte.fx_id}",
    "port": "Matte",
    "source_fx_id": "${steps.iris.fx_id}"
   }
  },
  {
   "id": "output",
   "tool": "fx.connect",
   "arguments": {
    "document_id": "${input.document_id}",
    "fx_id": "${steps.graph.current_output_fx_id}",
    "port": "source",
    "source_fx_id": "${steps.matte.fx_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "radius",
   "step": "iris",
   "parameter": "period"
  },
  {
   "name": "solid_radius",
   "step": "iris",
   "parameter": "innerperiod"
  }
 ],
 "seeds": [],
 "checks": [
  {
   "kind": "diagnostics_clean"
  },
  {
   "kind": "frames_differ",
   "frames": [
    0,
    6,
    12
   ]
  },
  {
   "kind": "render_alpha_nonempty",
   "frames": [
    12
   ]
  },
  {
   "kind": "reopen_pixels_equal",
   "frames": [
    6
   ]
  }
 ],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; frame_count is the length of the generator column in frames",
  "lengths": "not used",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "effect",
   "label": "editable",
   "from": "steps.matte.fx_id",
   "note": "The matte effect that carries the wipe, a native effect the client can keep editing."
  }
 ],
 "review": {
  "kind": "checks"
 },
 "limits": []
})json",
    R"json({
 "id": "dolly-shot-assembly",
 "version": 1,
 "title": "Dolly shot assembly",
 "outcome": "A new scene with one artwork column on a peg that dollies sideways with a breakdown, saved as a package and previewed.",
 "category": "shot",
 "brief": "Builds a shot from scratch: a scene with explicit size and frame rate, one imported PNG exposed as a hold, a peg the column hangs from, two linear keys on the peg's x channel and a breakdown between them. It saves an editable package copy and renders one reduced preview that must be read.",
 "required_types": [],
 "required_sources": [
  {
   "input": "artwork_path",
   "kind": "file"
  }
 ],
 "required_capabilities": {
  "tools": [
   "animation.insert_breakdown",
   "animation.sample",
   "animation.set_keys",
   "artifact.read",
   "column.create",
   "column.expose",
   "job.result",
   "job.status",
   "level.import",
   "object.create",
   "object.parent",
   "render.preview",
   "scene.create",
   "scene.save_copy",
   "scene.validate"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "draft",
   "file",
   "inspect",
   "job"
  ]
 },
 "preconditions": [
  "artwork_path names a PNG file under an input root the worker may read.",
  "directory names a new folder under a writable output root.",
  "breakdown_frame lies strictly between 0 and last_frame, and frame_count is last_frame + 1."
 ],
 "ownership": {
  "creates": [
   "scene",
   "level",
   "column",
   "dolly"
  ],
  "edits": [],
  "leaves_alone": "The recipe creates its own document; no existing document, level or stage object is read or changed."
 },
 "inputs": [
  {
   "name": "name",
   "schema": {
    "type": "string",
    "minLength": 1,
    "maxLength": 128,
    "default": "Dolly shot"
   }
  },
  {
   "name": "artwork_path",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "directory",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "frame_rate",
   "schema": {
    "type": "number",
    "minimum": 1,
    "maximum": 240,
    "default": 24
   }
  },
  {
   "name": "frame_count",
   "schema": {
    "type": "integer",
    "minimum": 2,
    "maximum": 10000,
    "default": 25
   }
  },
  {
   "name": "last_frame",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 9999,
    "default": 24
   }
  },
  {
   "name": "travel_inches",
   "schema": {
    "type": "number",
    "minimum": -100,
    "maximum": 100,
    "default": 0.5
   }
  },
  {
   "name": "breakdown_frame",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 9998,
    "default": 12
   }
  },
  {
   "name": "favour_percent",
   "schema": {
    "type": "number",
    "minimum": -100,
    "maximum": 200,
    "default": 25
   }
  }
 ],
 "steps": [
  {
   "id": "scene",
   "tool": "scene.create",
   "arguments": {
    "name": "${input.name}",
    "width_px": 64,
    "height_px": 64,
    "width_in": 0.5333333333333333,
    "height_in": 0.5333333333333333,
    "frame_rate": "${input.frame_rate}"
   }
  },
  {
   "id": "level",
   "tool": "level.import",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "path": "${input.artwork_path}",
    "name": "Artwork",
    "dpi": 120
   }
  },
  {
   "id": "column",
   "tool": "column.create",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "name": "Artwork"
   }
  },
  {
   "id": "expose",
   "tool": "column.expose",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "column_id": "${steps.column.column_id}",
    "level_id": "${steps.level.level_id}",
    "drawing_id": {
     "number": 1,
     "suffix": ""
    },
    "start_frame": 0,
    "length": "${input.frame_count}"
   }
  },
  {
   "id": "dolly",
   "tool": "object.create",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "name": "Dolly",
    "kind": "pegbar"
   }
  },
  {
   "id": "attach",
   "tool": "object.parent",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "object_id": "${steps.column.object_id}",
    "parent_id": "${steps.dolly.object_id}",
    "placement_policy": "preserve_local"
   }
  },
  {
   "id": "move",
   "tool": "animation.set_keys",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "object_id": "${steps.dolly.object_id}",
    "channel": "x",
    "unit": "inch",
    "keys": [
     {
      "frame": 0,
      "value": 0,
      "interpolation": "linear"
     },
     {
      "frame": "${input.last_frame}",
      "value": "${input.travel_inches}",
      "interpolation": "linear"
     }
    ]
   }
  },
  {
   "id": "breakdown",
   "tool": "animation.insert_breakdown",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "object_id": "${steps.dolly.object_id}",
    "channel": "x",
    "unit": "inch",
    "frame": "${input.breakdown_frame}",
    "favour_percent": "${input.favour_percent}"
   }
  },
  {
   "id": "samples",
   "tool": "animation.sample",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "object_id": "${steps.dolly.object_id}",
    "channel": "x",
    "unit": "inch",
    "frames": [
     0,
     "${input.breakdown_frame}",
     "${input.last_frame}"
    ]
   }
  },
  {
   "id": "validate",
   "tool": "scene.validate",
   "arguments": {
    "document_id": "${steps.scene.document_id}"
   }
  },
  {
   "id": "package",
   "tool": "scene.save_copy",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "directory": "${input.directory}"
   }
  },
  {
   "id": "preview",
   "tool": "render.preview",
   "arguments": {
    "document_id": "${steps.scene.document_id}",
    "frame": "${input.breakdown_frame}",
    "width": 32,
    "height": 32
   }
  },
  {
   "id": "preview_status",
   "tool": "job.status",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   },
   "poll": {
    "field": "status",
    "terminal": [
     "completed",
     "failed",
     "canceled",
     "deadline_exceeded"
    ],
    "success": [
     "completed"
    ],
    "max_attempts": 600
   }
  },
  {
   "id": "preview_result",
   "tool": "job.result",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   }
  },
  {
   "id": "preview_image",
   "tool": "artifact.read",
   "arguments": {
    "artifact_id": "${steps.preview_result.artifacts.0.artifact_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "travel",
   "step": "move",
   "input": "travel_inches"
  }
 ],
 "seeds": [],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; frame_count is last_frame + 1",
  "lengths": "inches (x travel), pixels and inches fixed by scene.create",
  "frame_rate": "frame_rate input, set by step scene"
 },
 "deliverables": [
  {
   "id": "scene",
   "label": "editable",
   "from": "steps.scene.document_id",
   "note": "The open draft document: columns, peg and keys the client can keep editing with the ordinary tools."
  },
  {
   "id": "package",
   "label": "editable",
   "from": "steps.package.package_directory",
   "note": "A native package copy of the scene that reopens as a draft."
  },
  {
   "id": "preview",
   "label": "approximated",
   "from": "steps.preview_result.artifacts.0.artifact_id",
   "note": "One frame at 32 by 32 pixels, smaller than the scene's 64 by 64."
  }
 ],
 "checks": [
  {
   "kind": "samples_differ",
   "step": "samples"
  },
  {
   "kind": "preview_alpha_nonempty",
   "frames": [
    12
   ]
  }
 ],
 "review": {
  "kind": "preview",
  "preview_step": "preview",
  "read_steps": [
   "preview_status",
   "preview_result",
   "preview_image"
  ],
  "frames": [
   12
  ],
  "required_before_success": true
 },
 "limits": []
})json",
    R"json({
 "id": "pendulum-rig",
 "version": 1,
 "title": "Pendulum rig",
 "outcome": "An existing column hangs from a pivoted peg that swings in a cycle, with a secondary sway on an offset layer.",
 "category": "rig",
 "brief": "Hangs an existing exposed column under a new peg, sets the peg's pivot, keys a rotation swing with eases, adds a breakdown, turns the cycle on so the swing repeats, and adds an offset layer whose sway adds to it. Samples show the repeat and the layer's share; one reduced preview must be read.",
 "required_types": [],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "animation.insert_breakdown",
   "animation.sample",
   "animation.set_cycle",
   "animation.set_keys",
   "artifact.read",
   "job.result",
   "job.status",
   "object.create",
   "object.parent",
   "object.pivot.set",
   "offset_layer.create",
   "render.preview"
  ],
  "toolsets": [
   "core",
   "rigging"
  ],
  "grants": [
   "draft",
   "inspect",
   "job"
  ]
 },
 "preconditions": [
  "document_id names an open draft document at its current revision.",
  "column_object_id names the stage object of an exposed column in it, with no parent peg to keep.",
  "half_period is at least 8, period is twice half_period, and breakdown_frame lies strictly between 0 and half_period."
 ],
 "ownership": {
  "creates": [
   "pendulum",
   "sway_layer"
  ],
  "edits": [
   "column_object_id"
  ],
  "leaves_alone": "Only the named column's parent changes; its own keys, other columns and effects are untouched."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "column_object_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "swing_start",
   "schema": {
    "type": "number",
    "minimum": -180,
    "maximum": 180,
    "default": -25
   }
  },
  {
   "name": "swing_end",
   "schema": {
    "type": "number",
    "minimum": -180,
    "maximum": 180,
    "default": 25
   }
  },
  {
   "name": "half_period",
   "schema": {
    "type": "integer",
    "minimum": 8,
    "maximum": 5000,
    "default": 12
   }
  },
  {
   "name": "period",
   "schema": {
    "type": "integer",
    "minimum": 16,
    "maximum": 10000,
    "default": 24
   }
  },
  {
   "name": "breakdown_frame",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 4999,
    "default": 4
   }
  },
  {
   "name": "pivot_x",
   "schema": {
    "type": "number",
    "minimum": -100,
    "maximum": 100,
    "default": 0
   }
  },
  {
   "name": "pivot_y",
   "schema": {
    "type": "number",
    "minimum": -100,
    "maximum": 100,
    "default": 0.25
   }
  },
  {
   "name": "sway_degrees",
   "schema": {
    "type": "number",
    "minimum": -180,
    "maximum": 180,
    "default": 5
   }
  }
 ],
 "steps": [
  {
   "id": "pendulum",
   "tool": "object.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "name": "Pendulum",
    "kind": "pegbar"
   }
  },
  {
   "id": "attach",
   "tool": "object.parent",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${input.column_object_id}",
    "parent_id": "${steps.pendulum.object_id}",
    "placement_policy": "preserve_local"
   }
  },
  {
   "id": "pivot",
   "tool": "object.pivot.set",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "unit": "inch",
    "center": {
     "x": "${input.pivot_x}",
     "y": "${input.pivot_y}"
    },
    "policy": {
     "kind": "keep_offset"
    },
    "coordinate_policy": "native_roundtrip",
    "max_coordinate_error_inch": 1e-06,
    "report_frames": [
     0
    ]
   }
  },
  {
   "id": "swing",
   "tool": "animation.set_keys",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "channel": "rotation",
    "unit": "degree",
    "keys": [
     {
      "frame": 0,
      "value": "${input.swing_start}",
      "interpolation": "ease_in_out",
      "outgoing": {
       "frames": 4
      }
     },
     {
      "frame": "${input.half_period}",
      "value": "${input.swing_end}",
      "interpolation": "ease_in_out",
      "incoming": {
       "kind": "ease",
       "frames": 4
      },
      "outgoing": {
       "frames": 4
      }
     },
     {
      "frame": "${input.period}",
      "value": "${input.swing_start}",
      "interpolation": "linear",
      "incoming": {
       "kind": "ease",
       "frames": 4
      }
     }
    ]
   }
  },
  {
   "id": "breakdown",
   "tool": "animation.insert_breakdown",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "channel": "rotation",
    "unit": "degree",
    "frame": "${input.breakdown_frame}",
    "favour_percent": 60
   }
  },
  {
   "id": "loop",
   "tool": "animation.set_cycle",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "channel": "rotation",
    "unit": "degree",
    "enabled": true
   }
  },
  {
   "id": "sway_layer",
   "tool": "offset_layer.create",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "name": "Secondary sway"
   }
  },
  {
   "id": "sway",
   "tool": "animation.set_keys",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "layer_id": "${steps.sway_layer.layer.layer_id}",
    "channel": "rotation",
    "unit": "degree",
    "keys": [
     {
      "frame": 0,
      "value": 0,
      "interpolation": "linear"
     },
     {
      "frame": "${input.half_period}",
      "value": "${input.sway_degrees}",
      "interpolation": "linear"
     }
    ]
   }
  },
  {
   "id": "samples",
   "tool": "animation.sample",
   "arguments": {
    "document_id": "${input.document_id}",
    "object_id": "${steps.pendulum.object_id}",
    "channel": "rotation",
    "unit": "degree",
    "frames": [
     6,
     12,
     30
    ]
   }
  },
  {
   "id": "preview",
   "tool": "render.preview",
   "arguments": {
    "document_id": "${input.document_id}",
    "frame": 6,
    "width": 32,
    "height": 32
   }
  },
  {
   "id": "preview_status",
   "tool": "job.status",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   },
   "poll": {
    "field": "status",
    "terminal": [
     "completed",
     "failed",
     "canceled",
     "deadline_exceeded"
    ],
    "success": [
     "completed"
    ],
    "max_attempts": 600
   }
  },
  {
   "id": "preview_result",
   "tool": "job.result",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   }
  },
  {
   "id": "preview_image",
   "tool": "artifact.read",
   "arguments": {
    "artifact_id": "${steps.preview_result.artifacts.0.artifact_id}"
   }
  }
 ],
 "controls": [
  {
   "name": "swing_end",
   "step": "swing",
   "input": "swing_end"
  },
  {
   "name": "sway",
   "step": "sway",
   "input": "sway_degrees"
  }
 ],
 "seeds": [],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; period is the swing length in frames and the cycle repeats it",
  "lengths": "inches (pivot); rotation in degrees",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "rig",
   "label": "editable",
   "from": "steps.pendulum.object_id",
   "note": "The peg with its pivot, cyclic swing and offset layer, all native stage content the client can keep editing."
  },
  {
   "id": "preview",
   "label": "approximated",
   "from": "steps.preview_result.artifacts.0.artifact_id",
   "note": "One frame at 32 by 32 pixels."
  }
 ],
 "checks": [
  {
   "kind": "cycle_repeats",
   "step": "samples",
   "frames": [
    6,
    30
   ]
  },
  {
   "kind": "layer_adds",
   "step": "samples",
   "frames": [
    12
   ]
  },
  {
   "kind": "preview_alpha_nonempty",
   "frames": [
    6
   ]
  }
 ],
 "review": {
  "kind": "preview",
  "preview_step": "preview",
  "read_steps": [
   "preview_status",
   "preview_result",
   "preview_image"
  ],
  "frames": [
   6
  ],
  "required_before_success": true
 },
 "limits": []
})json",
    R"json({
 "id": "frame-sequence-delivery",
 "version": 1,
 "title": "Frame sequence delivery",
 "outcome": "A checked scene is saved as a package copy, previewed small, and rendered to a PNG frame sequence that is read back.",
 "category": "delivery",
 "brief": "Validates the scene and preflights the sequence render, writes an editable package copy, renders one reduced preview that must be read, then renders the PNG frame sequence as private job artifacts and reads the first. It stops there: nothing publishes the frames to a project or shared output folder yet.",
 "required_types": [],
 "required_sources": [],
 "required_capabilities": {
  "tools": [
   "artifact.read",
   "job.result",
   "job.status",
   "render.preflight",
   "render.preview",
   "render.submit",
   "scene.save_copy",
   "scene.validate"
  ],
  "toolsets": [
   "core"
  ],
  "grants": [
   "file",
   "inspect",
   "job"
  ]
 },
 "preconditions": [
  "document_id names an open document with at least one exposed column, at its current revision.",
  "directory names a new folder under a writable output root.",
  "start_frame is not after end_frame, and preview_frame lies between them."
 ],
 "ownership": {
  "creates": [
   "package",
   "sequence"
  ],
  "edits": [],
  "leaves_alone": "The document is only read; no column, object or effect changes."
 },
 "inputs": [
  {
   "name": "document_id",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "directory",
   "schema": {
    "type": "string",
    "minLength": 1
   }
  },
  {
   "name": "start_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 0
   }
  },
  {
   "name": "end_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 3
   }
  },
  {
   "name": "width",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 2048,
    "default": 32
   }
  },
  {
   "name": "height",
   "schema": {
    "type": "integer",
    "minimum": 1,
    "maximum": 2048,
    "default": 32
   }
  },
  {
   "name": "preview_frame",
   "schema": {
    "type": "integer",
    "minimum": 0,
    "maximum": 1000000,
    "default": 0
   }
  }
 ],
 "steps": [
  {
   "id": "validate",
   "tool": "scene.validate",
   "arguments": {
    "document_id": "${input.document_id}"
   }
  },
  {
   "id": "preflight",
   "tool": "render.preflight",
   "arguments": {
    "document_id": "${input.document_id}",
    "kind": "sequence",
    "profile": "rgba8-png",
    "start_frame": "${input.start_frame}",
    "end_frame": "${input.end_frame}",
    "width": "${input.width}",
    "height": "${input.height}"
   }
  },
  {
   "id": "package",
   "tool": "scene.save_copy",
   "arguments": {
    "document_id": "${input.document_id}",
    "directory": "${input.directory}"
   }
  },
  {
   "id": "preview",
   "tool": "render.preview",
   "arguments": {
    "document_id": "${input.document_id}",
    "frame": "${input.preview_frame}",
    "width": 16,
    "height": 16
   }
  },
  {
   "id": "preview_status",
   "tool": "job.status",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   },
   "poll": {
    "field": "status",
    "terminal": [
     "completed",
     "failed",
     "canceled",
     "deadline_exceeded"
    ],
    "success": [
     "completed"
    ],
    "max_attempts": 600
   }
  },
  {
   "id": "preview_result",
   "tool": "job.result",
   "arguments": {
    "job_id": "${steps.preview.job_id}"
   }
  },
  {
   "id": "preview_image",
   "tool": "artifact.read",
   "arguments": {
    "artifact_id": "${steps.preview_result.artifacts.0.artifact_id}"
   }
  },
  {
   "id": "sequence",
   "tool": "render.submit",
   "arguments": {
    "document_id": "${input.document_id}",
    "profile": "rgba8-png",
    "start_frame": "${input.start_frame}",
    "end_frame": "${input.end_frame}",
    "width": "${input.width}",
    "height": "${input.height}"
   }
  },
  {
   "id": "sequence_status",
   "tool": "job.status",
   "arguments": {
    "job_id": "${steps.sequence.job_id}"
   },
   "poll": {
    "field": "status",
    "terminal": [
     "completed",
     "failed",
     "canceled",
     "deadline_exceeded"
    ],
    "success": [
     "completed"
    ],
    "max_attempts": 600
   }
  },
  {
   "id": "sequence_result",
   "tool": "job.result",
   "arguments": {
    "job_id": "${steps.sequence.job_id}"
   }
  }
 ],
 "controls": [],
 "seeds": [],
 "units": {
  "frame_origin": 0,
  "frames": "zero-based scene frames; start_frame and end_frame are both rendered",
  "lengths": "pixels",
  "frame_rate": "the scene's, unchanged"
 },
 "deliverables": [
  {
   "id": "package",
   "label": "editable",
   "from": "steps.package.package_directory",
   "note": "A native package copy that reopens as a draft."
  },
  {
   "id": "preview",
   "label": "approximated",
   "from": "steps.preview_result.artifacts.0.artifact_id",
   "note": "One frame at 16 by 16 pixels, smaller than the sequence."
  },
  {
   "id": "frames",
   "label": "baked",
   "from": "steps.sequence_result.artifacts.0.artifact_id",
   "note": "The first of the rendered PNG frames, a private job artifact; later artifacts follow it."
  }
 ],
 "checks": [
  {
   "kind": "preflight_passes",
   "step": "preflight"
  },
  {
   "kind": "frame_artifacts",
   "step": "sequence_result"
  },
  {
   "kind": "preview_alpha_nonempty",
   "frames": [
    0
   ]
  }
 ],
 "review": {
  "kind": "preview",
  "preview_step": "preview",
  "read_steps": [
   "preview_status",
   "preview_result",
   "preview_image"
  ],
  "frames": [
   0
  ],
  "required_before_success": true
 },
 "limits": [
  "Rendered frames stay private job artifacts of the worker; no tool publishes them to a project or shared output folder, so this recipe does not deliver files outside the worker."
 ]
})json"};

const QJsonArray& recipeDocuments() {
  static const QJsonArray documents = [] {
    QJsonArray parsed;
    for (const char* text : recipeTexts) {
      QJsonParseError error;
      const QJsonDocument document =
          QJsonDocument::fromJson(QByteArray(text), &error);
      if (error.error != QJsonParseError::NoError || !document.isObject())
        throw Error("internal_error", "An embedded recipe is malformed.");
      parsed.append(document.object());
    }
    return parsed;
  }();
  return documents;
}

QJsonObject withAvailability(QJsonObject recipe) {
  QJsonArray missing;
  for (const QJsonValue& type : recipe.value("required_types").toArray())
    if (!isAutomationFxTypeAvailable(type.toString())) missing.append(type);
  recipe.insert("available", missing.isEmpty());
  recipe.insert("missing_types", missing);
  return recipe;
}

}  // namespace

QJsonArray automationFxRecipes() { return recipeDocuments(); }

QJsonValue listAutomationFxRecipes(const QJsonObject& params) {
  QJsonArray recipes;
  for (const QJsonValue& value : recipeDocuments()) {
    const QJsonObject recipe = value.toObject();
    if (params.contains("recipe_id") &&
        recipe.value("id") != params.value("recipe_id"))
      continue;
    if (params.contains("category") &&
        recipe.value("category") != params.value("category"))
      continue;
    recipes.append(withAvailability(recipe));
  }
  return QJsonObject{{"profile", automationFxProfileName()},
                     {"recipes", recipes}};
}

}  // namespace GroundzeroAutomation
