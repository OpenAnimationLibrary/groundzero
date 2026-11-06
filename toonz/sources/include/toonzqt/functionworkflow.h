#pragma once
class QWidget;
class FunctionViewer;
class FunctionTreeView;
class FunctionPanel;
QWidget *createFunctionWorkflowControls(FunctionViewer *viewer,
                                        FunctionTreeView *tree,
                                        FunctionPanel *graph);
