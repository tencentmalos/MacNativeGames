// SPDX-License-Identifier: MIT
// State packing adapted from MacNativeGames' MIT native NVN frontend
// games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c at a02228e.
// These small by-value states deliberately use the renderer's packed layout;
// unlike resource objects they are copied directly by the guest.
static bool nvn_bool(uint32_t value) { return (value & 255) != 0; }
#define ST(T, x) ((T *)(x))
API(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
API(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
API(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
API(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
API(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
API(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) {need(op==NVN_LOGIC_OP_COPY,"Non-copy logic operation unsupported");ST(color_st,s)->logic_op=op;}
API(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) {need(f==NVN_ALPHA_FUNC_ALWAYS,"Alpha test emulation unavailable");ST(color_st,s)->alpha_func=f;}
API(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); need(i >= 0 && i <= 7, "Color attachment index");
    o->blend_en = nvn_bool(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
API(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
API(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    need(i >= 0 && i <= 7, "Color attachment index");
    uint32_t m = (nvn_bool(r) ? 1 : 0) | (nvn_bool(g) ? 2 : 0) | (nvn_bool(b) ? 4 : 0) | (nvn_bool(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
API(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
API(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = nvn_bool(e) != 0; }
API(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
API(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = nvn_bool(e) != 0; }
API(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { need(!nvn_bool(e), "Programmable sample locations unavailable"); ST(ms_st, s)->sl_en = 0; }
API(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)v; need(n == 0, "Programmable sample locations unavailable"); }
API(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
API(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
API(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
API(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
API(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
API(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
API(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = nvn_bool(e) != 0; }
API(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = nvn_bool(e) != 0; }
API(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
API(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = nvn_bool(e) != 0; }
API(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
API(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
API(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
API(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    need(off >= 0 && off < (1 << 19), "Vertex attribute offset");
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
API(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
API(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
API(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
API(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }


#undef ST
template<class T, class U> void bind_state(NVNcommandBuffer *c, int kind, const U *state) {
    COMMAND_LOCK;static_assert(sizeof(T)<=sizeof(U));need(state,"Null render state");
    p_bind_state p{};p.kind=kind;memcpy(p.bytes,state,sizeof(T));record(c,OP_BIND_STATE,p);
}
API(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *state) { bind_state<blend_st>(c,0,state); }
API(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *state) { bind_state<color_st>(c,1,state); }
API(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *state) { bind_state<chmask_st>(c,2,state); }
API(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *state) { bind_state<ms_st>(c,3,state); }
API(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *state) { bind_state<poly_st>(c,4,state); }
API(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *state) { bind_state<ds_st>(c,5,state); }
API(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *state) {
    COMMAND_LOCK;need(n>=0 && n<=16 && (!n || state),"Vertex attribute state count");
    static_assert(sizeof(NVNvertexAttribState)==4);p_bind_vattr p{};p.n=n;
    memcpy(p.st,state,n*4);record(c,OP_BIND_VATTR,p);
}
API(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *state) {
    COMMAND_LOCK;need(n>=0 && n<=16 && (!n || state),"Vertex stream state count");
    static_assert(sizeof(NVNvertexStreamState)==8);p_bind_vstream p{};p.n=n;
    memcpy(p.st,state,n*8);record(c,OP_BIND_VSTREAM,p);
}
