#include "automation_vector_opacity.h"

#include "tstroke.h"
#include "tstrokeopacity.h"
#include "toonz/txshsimplelevel.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QJsonDocument>
#include <cmath>

namespace GroundzeroAutomation {
namespace {
constexpr char Schemas[] = R"json(
[
  {
    "name": "vector.select",
    "writes": false,
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "document_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "level_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "drawing_id": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "number": {
              "type": "integer",
              "minimum": 1,
              "maximum": 65535
            },
            "suffix": {
              "type": "string",
              "pattern": "^[a-z]{0,8}$"
            }
          },
          "required": [
            "number",
            "suffix"
          ]
        },
        "expected_revision": {
          "type": "string",
          "minLength": 1,
          "maxLength": 20,
          "pattern": "^(0|[1-9][0-9]*)$"
        },
        "coordinate_space": {
          "type": "string",
          "enum": [
            "drawing_native"
          ]
        },
        "query": {
          "oneOf": [
            {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "mode": {
                  "type": "string",
                  "enum": [
                    "indices"
                  ]
                },
                "stroke_indices": {
                  "type": "array",
                  "items": {
                    "type": "integer",
                    "minimum": 0,
                    "maximum": 4095
                  },
                  "minItems": 1,
                  "maxItems": 128
                }
              },
              "required": [
                "mode",
                "stroke_indices"
              ]
            },
            {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "mode": {
                  "type": "string",
                  "enum": [
                    "nearest_centerline"
                  ]
                },
                "x": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                },
                "y": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                },
                "max_distance": {
                  "type": "number",
                  "minimum": 0,
                  "maximum": 32768
                }
              },
              "required": [
                "mode",
                "x",
                "y",
                "max_distance"
              ]
            },
            {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "mode": {
                  "type": "string",
                  "enum": [
                    "centerline_bounds"
                  ]
                },
                "relation": {
                  "type": "string",
                  "enum": [
                    "intersects",
                    "contained"
                  ]
                },
                "x0": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                },
                "y0": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                },
                "x1": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                },
                "y1": {
                  "type": "number",
                  "minimum": -32768,
                  "maximum": 32768
                }
              },
              "required": [
                "mode",
                "relation",
                "x0",
                "y0",
                "x1",
                "y1"
              ]
            },
            {
              "type": "object",
              "additionalProperties": false,
              "properties": {
                "mode": {
                  "type": "string",
                  "enum": [
                    "lasso"
                  ]
                },
                "relation": {
                  "type": "string",
                  "enum": [
                    "intersects",
                    "contained"
                  ]
                },
                "points": {
                  "type": "array",
                  "items": {
                    "type": "object",
                    "additionalProperties": false,
                    "properties": {
                      "x": {
                        "type": "number",
                        "minimum": -32768,
                        "maximum": 32768
                      },
                      "y": {
                        "type": "number",
                        "minimum": -32768,
                        "maximum": 32768
                      }
                    },
                    "required": [
                      "x",
                      "y"
                    ]
                  },
                  "minItems": 3,
                  "maxItems": 256
                }
              },
              "required": [
                "mode",
                "relation",
                "points"
              ]
            }
          ]
        },
        "roles": {
          "type": "array",
          "items": {
            "type": "string",
            "enum": [
              "line_art",
              "colour_art",
              "underlay",
              "overlay"
            ]
          },
          "minItems": 1,
          "maxItems": 4
        }
      },
      "required": [
        "document_id",
        "level_id",
        "drawing_id",
        "expected_revision",
        "coordinate_space",
        "query"
      ]
    }
  },
  {
    "name": "vector.opacity.inspect",
    "writes": false,
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "document_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "level_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "drawing_id": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "number": {
              "type": "integer",
              "minimum": 1,
              "maximum": 65535
            },
            "suffix": {
              "type": "string",
              "pattern": "^[a-z]{0,8}$"
            }
          },
          "required": [
            "number",
            "suffix"
          ]
        },
        "expected_revision": {
          "type": "string",
          "minLength": 1,
          "maxLength": 20,
          "pattern": "^(0|[1-9][0-9]*)$"
        },
        "stroke_index": {
          "type": "integer",
          "minimum": 0,
          "maximum": 4095
        },
        "offset": {
          "type": "integer",
          "minimum": 0,
          "maximum": 4096
        },
        "limit": {
          "type": "integer",
          "minimum": 1,
          "maximum": 128
        }
      },
      "required": [
        "document_id",
        "level_id",
        "drawing_id",
        "expected_revision",
        "stroke_index",
        "offset",
        "limit"
      ]
    }
  },
  {
    "name": "vector.opacity.set",
    "writes": true,
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "document_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "level_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "drawing_id": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "number": {
              "type": "integer",
              "minimum": 1,
              "maximum": 65535
            },
            "suffix": {
              "type": "string",
              "pattern": "^[a-z]{0,8}$"
            }
          },
          "required": [
            "number",
            "suffix"
          ]
        },
        "expected_revision": {
          "type": "string",
          "minLength": 1,
          "maxLength": 20,
          "pattern": "^(0|[1-9][0-9]*)$"
        },
        "operation_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "sharing": {
          "type": "string",
          "enum": [
            "all_exposures"
          ]
        },
        "stroke_indices": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0,
            "maximum": 4095
          },
          "minItems": 1,
          "maxItems": 128
        },
        "selection_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "opacity_per_mille": {
          "type": "integer",
          "minimum": 0,
          "maximum": 1000
        }
      },
      "required": [
        "document_id",
        "level_id",
        "drawing_id",
        "expected_revision",
        "operation_id",
        "sharing",
        "opacity_per_mille"
      ]
    }
  },
  {
    "name": "vector.opacity.profile.set",
    "writes": true,
    "inputSchema": {
      "type": "object",
      "additionalProperties": false,
      "properties": {
        "document_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "level_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "drawing_id": {
          "type": "object",
          "additionalProperties": false,
          "properties": {
            "number": {
              "type": "integer",
              "minimum": 1,
              "maximum": 65535
            },
            "suffix": {
              "type": "string",
              "pattern": "^[a-z]{0,8}$"
            }
          },
          "required": [
            "number",
            "suffix"
          ]
        },
        "expected_revision": {
          "type": "string",
          "minLength": 1,
          "maxLength": 20,
          "pattern": "^(0|[1-9][0-9]*)$"
        },
        "operation_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "sharing": {
          "type": "string",
          "enum": [
            "all_exposures"
          ]
        },
        "stroke_indices": {
          "type": "array",
          "items": {
            "type": "integer",
            "minimum": 0,
            "maximum": 4095
          },
          "minItems": 1,
          "maxItems": 128
        },
        "selection_id": {
          "type": "string",
          "minLength": 1,
          "maxLength": 128
        },
        "knots": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": false,
            "properties": {
              "position": {
                "type": "number",
                "minimum": 0,
                "maximum": 1
              },
              "opacity": {
                "type": "number",
                "minimum": 0,
                "maximum": 1
              }
            },
            "required": [
              "position",
              "opacity"
            ]
          },
          "minItems": 0,
          "maxItems": 4096
        }
      },
      "required": [
        "document_id",
        "level_id",
        "drawing_id",
        "expected_revision",
        "operation_id",
        "sharing",
        "knots"
      ]
    }
  }
]
)json";

QByteArray strokeBytes(const TStroke& stroke, int index) {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream.setByteOrder(QDataStream::BigEndian);
  stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
  const auto& knots = stroke.getOpacityProfile();
  stream << qint32(index) << qint32(nativeOpacityPerMille(stroke))
         << quint32(knots.size());
  for (const auto& knot : knots)
    stream << (knot.x == 0 ? 0.0 : knot.x) << (knot.y == 0 ? 0.0 : knot.y);
  return bytes;
}
}  // namespace

int nativeOpacityPerMille(const TStroke& stroke) {
  const double value = stroke.getOpacity();
  if (!std::isfinite(value) || value < 0 || value > 1)
    throw Error("unsupported_type",
                "Native stroke opacity must be finite in [0,1].");
  const int units = static_cast<int>(std::round(value * 1000));
  if (value != units / 1000.0)
    throw Error("unsupported_type",
                "Native stroke opacity must use exact per-mille units.");
  return units;
}

void validateVectorOpacity(const TVectorImageP& image) {
  size_t total = 0;
  for (UINT index = 0; index < image->getStrokeCount(); ++index) {
    const auto& stroke = *image->getStroke(index);
    nativeOpacityPerMille(stroke);
    const auto& knots = stroke.getOpacityProfile();
    total += knots.size();
    if (knots.size() > 4096 || total > 65536)
      throw Error("resource_limit",
                  "Opacity profiles admit 4096 knots per stroke and 65536 per "
                  "drawing.");
    if (!StrokeOpacity::valid(knots))
      throw Error("unsupported_type",
                  "Native stroke opacity profile is malformed.");
  }
}

QString vectorLevelOpacityDigest(TXshSimpleLevel& level) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData("groundzero-vector-opacity-v1", 27);
  for (const auto& frame : level.getFids()) {
    const TVectorImageP image = level.getFrame(frame, false);
    if (!image)
      throw Error("asset_unavailable", "Cannot digest missing vector drawing.");
    validateVectorOpacity(image);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::BigEndian);
    const QByteArray suffix = frame.getLetter().toUtf8();
    stream << qint32(frame.getNumber()) << quint32(suffix.size());
    stream.writeRawData(suffix.constData(), suffix.size());
    stream << quint32(image->getStrokeCount());
    hash.addData(bytes);
    for (UINT index = 0; index < image->getStrokeCount(); ++index)
      hash.addData(strokeBytes(*image->getStroke(index), index));
  }
  return QString::fromLatin1(hash.result().toHex());
}

QJsonArray vectorOpacityOperations() {
  QJsonArray result;
  const QStringList descriptions{
      "Select bounded revision-bound stroke indices using explicit indices, "
      "centerline geometry or a lasso, optionally limited to art roles, and "
      "return a selection_id handle bound to the revision.",
      "Inspect native per-mille stroke opacity and a bounded envelope page.",
      "Set native per-mille ink opacity while preserving stroke envelopes and "
      "region paint; provide exactly one of stroke_indices and selection_id.",
      "Replace native ink opacity envelopes while preserving scalar opacity "
      "and region paint; provide exactly one of stroke_indices and "
      "selection_id."};
  const auto definitions = QJsonDocument::fromJson(Schemas).array();
  for (int index = 0; index < definitions.size(); ++index) {
    const auto definition = definitions[index].toObject();
    const auto schema     = definition.value("inputSchema").toObject();
    auto descriptor =
        operation(definition.value("name").toString(), descriptions[index],
                  schema.value("properties").toObject(),
                  schema.value("required").toArray(),
                  definition.value("writes").toBool());
    descriptor.insert("inputSchema", schema);
    result.append(descriptor);
  }
  return result;
}
}  // namespace GroundzeroAutomation
