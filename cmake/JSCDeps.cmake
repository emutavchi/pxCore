if (PREFER_SYSTEM_LIBRARIES)

# pkg_search_module(JAVASCRIPTCORE javascriptcoregtk-4.0)

pkg_search_module(GLIB glib-2.0 )

 set(JAVASCRIPTCORE_INCLUDE_DIRS
     "/projects/generic/jsc_2018/Source"
     "/projects/generic/jsc_2018/build/DerivedSources/ForwardingHeaders"
 )
 set(JAVASCRIPTCORE_LIBRARIES -L/projects/generic/jsc_2018/build/lib -lJavaScriptCore -licuuc -licui18n)

 set(JAVASCRIPTCORE_INCLUDE_DIRS ${JAVASCRIPTCORE_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
 set(JAVASCRIPTCORE_LIBRARIES ${JAVASCRIPTCORE_LIBRARIES} ${GLIB_LIBRARIES})

message("JAVASCRIPTCORE: ${JAVASCRIPTCORE_INCLUDE_DIRS}")

endif(PREFER_SYSTEM_LIBRARIES)
