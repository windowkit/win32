'use strict';

// Does the GL surface actually put pixels on the screen? Asked with
// glReadPixels rather than with a screenshot, so the answer is a number.
//
// This is the test that separates "the GL path runs" from "the GL path draws",
// which a blank map cannot distinguish: a renderer with no geometry and a
// renderer whose output goes nowhere look identical.

const assert = require('node:assert');
const win32 = require('../index.js');

const events = [];
win32.start((e) => events.push(e));

const waitFor = (type, ms = 5000) =>
  new Promise((resolve, reject) => {
    const started = Date.now();
    const tick = setInterval(() => {
      const found = events.find((e) => e.type === type);
      if (found) {
        clearInterval(tick);
        resolve(found);
      } else if (Date.now() - started > ms) {
        clearInterval(tick);
        reject(new Error(`no ${type} within ${ms}ms`));
      }
    }, 25);
  });

const VERTEX = `#version 330 core
layout(location = 0) in vec2 a_pos;
void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }
`;

const FRAGMENT = `#version 330 core
out vec4 fragColor;
void main() { fragColor = vec4(1.0, 0.5, 0.0, 1.0); }
`;

async function main() {
  const probe = win32.glProbe();
  console.log(`driver    : ${probe.renderer}`);
  console.log(`version   : ${probe.version} ${probe.core ? '(core)' : ''}`);
  assert.ok(probe.core, 'no core context — the software rasterizer cannot run this');

  const wnd = win32.createWindow({ title: 'gl test', width: 320, height: 240 });
  await waitFor('window-ready');
  win32.compose(wnd);
  win32.show(wnd, true);

  const surface = win32.glCreateSurface(win32.windowHandle(wnd), 0, 0, 320, 240);
  assert.ok(surface > 0, 'glCreateSurface answered no id');
  const ready = await waitFor('gl-ready');
  assert.equal(ready.a, 1, 'the GL surface was refused');
  console.log('surface   : created on the UI thread, context made here');

  assert.ok(win32.glMakeCurrent(surface), 'could not make the context current');
  const gl = win32.glTable();

  // --- a clear, read back -----------------------------------------------------
  gl.viewport(0, 0, 320, 240);
  gl.clearColor(0.1, 0.6, 0.3, 1);
  gl.clear(0x4000 /* COLOR_BUFFER_BIT */);
  gl.finish();

  const pixels = new Uint8Array(4);
  gl.readPixels(160, 120, 1, 1, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, pixels);
  console.log(`clear     : centre pixel ${[...pixels].join(',')}`);
  assert.ok(Math.abs(pixels[0] - 26) < 4, 'red channel is not the clear colour');
  assert.ok(Math.abs(pixels[1] - 153) < 4, 'green channel is not the clear colour');
  assert.ok(Math.abs(pixels[2] - 77) < 4, 'blue channel is not the clear colour');

  // --- a shader, a buffer, a VAO, a draw --------------------------------------
  // This is the part that proves it is a *core* context and not GL 1.1: none
  // of these exist there.
  const vs = gl.createShader(0x8b31 /* VERTEX_SHADER */);
  gl.shaderSource(vs, VERTEX);
  gl.compileShader(vs);
  assert.ok(
    gl.getShaderParameter(vs, 0x8b81 /* COMPILE_STATUS */),
    `vertex shader: ${gl.getShaderInfoLog(vs)}`,
  );

  const fs = gl.createShader(0x8b30 /* FRAGMENT_SHADER */);
  gl.shaderSource(fs, FRAGMENT);
  gl.compileShader(fs);
  assert.ok(
    gl.getShaderParameter(fs, 0x8b81),
    `fragment shader: ${gl.getShaderInfoLog(fs)}`,
  );

  const program = gl.createProgram();
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  assert.ok(
    gl.getProgramParameter(program, 0x8b82 /* LINK_STATUS */),
    `link: ${gl.getProgramInfoLog(program)}`,
  );
  console.log('shaders   : compiled and linked');

  const vao = gl.createVertexArray();
  gl.bindVertexArray(vao);
  const buffer = gl.createBuffer();
  gl.bindBuffer(0x8892 /* ARRAY_BUFFER */, buffer);
  // A triangle covering the middle of the viewport.
  gl.bufferData(
    0x8892,
    new Float32Array([-0.8, -0.8, 0.8, -0.8, 0.0, 0.8]),
    0x88e4 /* STATIC_DRAW */,
  );
  gl.enableVertexAttribArray(0);
  gl.vertexAttribPointer(0, 2, 0x1406 /* FLOAT */, false, 0, 0);

  gl.useProgram(program);
  gl.drawArrays(4 /* TRIANGLES */, 0, 3);
  gl.finish();

  const drawn = new Uint8Array(4);
  gl.readPixels(160, 100, 1, 1, 0x1908, 0x1401, drawn);
  console.log(`triangle  : centre pixel ${[...drawn].join(',')}`);
  assert.ok(drawn[0] > 240, 'the triangle did not draw — red channel');
  assert.ok(Math.abs(drawn[1] - 128) < 6, 'the triangle did not draw — green channel');
  assert.equal(drawn[2], 0, 'the triangle did not draw — blue channel');

  assert.equal(gl.getError(), 0, 'GL reported an error');

  win32.glSwapBuffers(surface);
  win32.glDestroySurface(surface);
  win32.stop();
  console.log('\nok — a core context, shaders, a VAO and a draw, verified in pixels');
  process.exit(0);
}

main().catch((err) => {
  console.error('FAILED:', err.message);
  win32.stop();
  process.exit(1);
});
