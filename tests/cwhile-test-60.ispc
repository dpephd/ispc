
export uniform int width() { return programCount; }


export void f_fu(uniform float RET[], uniform float aFOO[], uniform float b) {
    float a = aFOO[programIndex];
    cwhile (a < 10)
        ++a;
    RET[programIndex] = a;
}


export void result(uniform float RET[]) {
    RET[programIndex] = 10;
}
