/*
    methods.tst - Test various http methods
 */

load("support.es")

//  Options/Trace
run("--method OPTIONS /index.html")
assert(run("--showCode -q --method TRACE /index.html").contains("406"))
