#pragma once

#ifndef TVECTORGL_INCLUDED
#define TVECTORGL_INCLUDED

#ifdef _WIN32
#include <windows.h>
#include <cstdlib>

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/glut.h>
#endif

#ifdef MACOSX
#include <GLUT/glut.h>
#endif

//#include "tgeometry.h"
#include "tcommon.h"
#include <string>

#undef DVAPI
#undef DVVAR

#ifdef DV_LOCAL_DEFINED
#define DVAPI
#define DVVAR
#else
#ifdef TVRENDER_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif
#endif

//=============================================================================
// forward declarations
class TStroke;
class TRegion;
class TVectorImage;
class TVectorRenderData;

//=============================================================================

// Widget paint events and explicit widget framebuffer captures create this
// scope. Captures throw after Qt finishes if the scope recorded a failure.
// Native offscreen/file rendering retains its throwing failure contract.
// Nested widget paints collect their own errors and restore the parent scope.
class DVAPI TViewerOpacityErrorScope final {
  TViewerOpacityErrorScope *m_previous;
  std::string m_error;
public:
  TViewerOpacityErrorScope();
  ~TViewerOpacityErrorScope();
  TViewerOpacityErrorScope(const TViewerOpacityErrorScope &) = delete;
  TViewerOpacityErrorScope &operator=(const TViewerOpacityErrorScope &) = delete;
  const std::string &error() const { return m_error; }
  void record(const char *message);
  static bool currentFailed();
};

//=============================================================================

/*!
  Draw a stroke in a GL context.
  \par stroke is element to draw
  \par rd: \sa TVectorRenderData
 */
void DVAPI tglDraw(const TVectorRenderData &rd, const TVectorImage *vim,
                   TStroke **guidedStroke = (TStroke **)nullptr);
void DVAPI tglDrawMask(const TVectorRenderData &rd, const TVectorImage *vim);
void DVAPI tglDraw(const TVectorRenderData &rd, const TStroke *stroke,
                   bool pushAttribs = true);
void DVAPI tglDraw(const TVectorRenderData &rd, TRegion *r,
                   bool pushAttribs = true);

#ifdef BUILD_UI_PORT_TESTS
// Native acceptance fault injection, absent from production builds. 0 resets,
// 1 simulates missing FBO entry points, 2 fails after resource allocation,
// 3 fails only inside an intermediate style capture.
void DVAPI tglSetOpacityFramebufferFailureForTest(int stage);
void DVAPI tglSetOpacityViewportCaptureForTest(bool full);
#endif

//=============================================================================

#endif
