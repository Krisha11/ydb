#pragma once

#ifdef USE_PYTHON3
#include <contrib/tools/python3/Include/cpython/fileobject.h>
#else
#error "No <cpython/fileobject.h> in Python2"
#endif
