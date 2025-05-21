# 查找 include 路径
find_path(FAAC_INCLUDE_DIR
  NAMES faac.h
  PATHS "D:\\kkem\\test\\faac\\include"
)

# 查找库文件
find_library(FAAC_LIBRARY
  NAMES faac
  PATHS "D:\\kkem\\test\\faac\\lib"
)

set(FAAC_INCLUDE_DIRS ${FAAC_INCLUDE_DIR})
set(FAAC_LIBRARIES ${FAAC_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FAAC DEFAULT_MSG FAAC_LIBRARY FAAC_INCLUDE_DIR)
