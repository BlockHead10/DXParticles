#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <vector>
#include <cstdlib>
#include <cmath>
#include <ctime>
#pragma comment(lib, "d3d11.lib")
#include <d3dcompiler.h>
#pragma comment(lib, "D3dcompiler.lib")
#include <algorithm>

using namespace DirectX;

// --- DirectX globals ---
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pImmediateContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// --- Window size ---
const int g_width = 800;
const int g_height = 600;

// --- Particle struct ---
struct Particle3D
{
    XMFLOAT3 pos;
    XMFLOAT3 vel;
};

// --- Particle settings ---
std::vector<Particle3D> particles;
const int numParticles = 400;
const float maxDepth = 100.0f;
const float particleSpeed = 40.0f;
const float particleLineDist = 40.0f;

// --- Vertex for lines ---
struct Vertex
{
    XMFLOAT3 pos;
    XMFLOAT4 color;
};

// --- Camera ---
XMMATRIX g_view;
XMMATRIX g_proj;
float g_rotationAngle = 0.0f;  // horizontal rotation (around Y)
float g_pitchAngle = 0.0f;     // vertical rotation (tilt up/down)
float g_camDistance = 600.0f;  // distance from cube center
static float g_radius = 600.0f;
const float g_minRadius = 100.0f;
const float g_maxRadius = 1500.0f;


// --- Shaders (HLSL compiled at runtime) ---
const char* g_VSCode = R"(
cbuffer MatrixBuffer : register(b0)
{
    matrix world;
    matrix view;
    matrix projection;
};

struct VS_INPUT
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;
    float4 worldPos = mul(float4(input.pos,1), world);
    float4 viewPos  = mul(worldPos, view);
    output.pos      = mul(viewPos, projection);
    output.color    = input.color;
    return output;
}
)";

const char* g_PSCode = R"(
struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PS_INPUT input) : SV_TARGET
{
    return input.color;
}
)";

// --- DirectX objects ---
ID3D11InputLayout* g_pInputLayout = nullptr;
ID3D11VertexShader* g_pVertexShader = nullptr;
ID3D11PixelShader* g_pPixelShader = nullptr;
ID3D11Buffer* g_pVertexBuffer = nullptr;
ID3D11Buffer* g_pMatrixBuffer = nullptr;

// --- Simple compile helper ---
HRESULT CompileShaderFromMemory(const char* src, const char* entry, const char* target, ID3DBlob** blob)
{
    return D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, blob, nullptr);
}

// --- Initialize DX ---
bool InitD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = g_width;
    sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    HRESULT hrCreate = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pImmediateContext
    );
    if (FAILED(hrCreate)) return false;

    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    g_pImmediateContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    D3D11_VIEWPORT vp = {};
    vp.Width = g_width;
    vp.Height = g_height;
    vp.MaxDepth = 1.0f;
    g_pImmediateContext->RSSetViewports(1, &vp);

    // --- Compile shaders ---
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(g_VSCode, strlen(g_VSCode), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        if (errorBlob) errorBlob->Release();
        return false;
    }

    hr = D3DCompile(g_PSCode, strlen(g_PSCode), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        if (errorBlob) errorBlob->Release();
        return false;
    }

    hr = g_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    if (FAILED(hr)) return false;

    hr = g_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pPixelShader);
    if (FAILED(hr)) return false;

    // --- Input layout ---
    D3D11_INPUT_ELEMENT_DESC layoutDesc[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    g_pd3dDevice->CreateInputLayout(layoutDesc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_pInputLayout);
    vsBlob->Release();
    psBlob->Release();

    // --- Matrix buffer ---
    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(XMMATRIX) * 3;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_pd3dDevice->CreateBuffer(&cbd, nullptr, &g_pMatrixBuffer);

    // --- Projection ---
    g_proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, g_width / (float)g_height, 1.0f, 1000.0f);

    return true;
}

// --- Initialize particles ---
void InitParticles()
{
    particles.resize(numParticles);
    for (auto& p : particles)
    {
        float cubeSize = 200.0f;  // cube spans -100 +100 in all axes
        p.pos = XMFLOAT3(
            ((float)rand() / RAND_MAX - 0.5f) * cubeSize,
            ((float)rand() / RAND_MAX - 0.5f) * cubeSize,
            ((float)rand() / RAND_MAX - 0.5f) * cubeSize
        );
        p.vel = XMFLOAT3(
            ((float)rand() / RAND_MAX - 0.5f) * particleSpeed,
            ((float)rand() / RAND_MAX - 0.5f) * particleSpeed,
            ((float)rand() / RAND_MAX - 0.5f) * particleSpeed
        );

    }
}

// --- Update particles ---
void UpdateParticles(float dt)
{
    float halfSize = 150.0f; // cube extends from -halfSize to +halfSize on all axes

    for (auto& p : particles)
    {
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.pos.z += p.vel.z * dt;

        // Reflect/bounce off cube walls
        if (p.pos.x < -halfSize || p.pos.x > halfSize) p.vel.x *= -1;
        if (p.pos.y < -halfSize || p.pos.y > halfSize) p.vel.y *= -1;
        if (p.pos.z < -halfSize || p.pos.z > halfSize) p.vel.z *= -1;

        // Clamp in case velocity overshoots
        p.pos.x = std::max(-halfSize, std::min(p.pos.x, halfSize));
        p.pos.y = std::max(-halfSize, std::min(p.pos.y, halfSize));
        p.pos.z = std::max(-halfSize, std::min(p.pos.z, halfSize));
    }
}

// --- Create line vertices ---
void BuildLineVertices(std::vector<Vertex>& verts)
{
    verts.clear();
    for (size_t i = 0; i < particles.size(); ++i)
    {
        for (size_t j = i + 1; j < particles.size(); ++j)
        {
            XMFLOAT3& a = particles[i].pos;
            XMFLOAT3& b = particles[j].pos;
            float dx = b.x - a.x;
            float dy = b.y - a.y;
            float dz = b.z - a.z;
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < particleLineDist)
            {
                float alpha = 1.0f - dist / particleLineDist;
                XMFLOAT4 color(1, 1, 1, alpha);
                verts.push_back({ a,color });
                verts.push_back({ b,color });
            }
        }
    }
}

// --- Win32 WndProc ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_radius -= delta * 0.25f; // adjust sensitivity
        if (g_radius < g_minRadius) g_radius = g_minRadius;
        if (g_radius > g_maxRadius) g_radius = g_maxRadius;
    }
    return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// --- Main ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    srand((unsigned int)time(0));

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0,0,
                      hInstance,nullptr,nullptr,nullptr,nullptr,
                      L"DXParticles", nullptr };
    RegisterClassEx(&wc);

    HWND hWnd = CreateWindow(L"DXParticles", L"3D Particle Network",
        WS_OVERLAPPEDWINDOW, 100, 100, g_width, g_height,
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);

    if (!InitD3D(hWnd)) return 1;

    InitParticles();

    // Vertex buffer
    D3D11_BUFFER_DESC vbd = {};
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.ByteWidth = sizeof(Vertex) * numParticles * numParticles * 2; // max possible lines
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_pd3dDevice->CreateBuffer(&vbd, nullptr, &g_pVertexBuffer);

    MSG msg = {};
    DWORD lastTime = GetTickCount();

    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            DWORD now = GetTickCount();
            float dt = (now - lastTime) / 1000.0f;
            lastTime = now;

            // Update particles
            UpdateParticles(dt);

            // Build line vertices
            std::vector<Vertex> verts;
            BuildLineVertices(verts);

            // Update vertex buffer
            D3D11_MAPPED_SUBRESOURCE ms;
            g_pImmediateContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            memcpy(ms.pData, verts.data(), verts.size() * sizeof(Vertex));
            g_pImmediateContext->Unmap(g_pVertexBuffer, 0);

            // Clear
            float clearColor[4] = { 0,0,0,1 };
            g_pImmediateContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);

            // Set pipeline
            UINT stride = sizeof(Vertex);
            UINT offset = 0;
            g_pImmediateContext->IASetInputLayout(g_pInputLayout);
            g_pImmediateContext->IASetVertexBuffers(0, 1, &g_pVertexBuffer, &stride, &offset);
            g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            g_pImmediateContext->VSSetShader(g_pVertexShader, nullptr, 0);
            g_pImmediateContext->PSSetShader(g_pPixelShader, nullptr, 0);

            // --- Update matrices ---
            XMMATRIX world = XMMatrixIdentity();

            // --- Camera orbit parameters ---
   

            // --- Convert spherical coords to Cartesian for orbiting camera ---
            float camX = sinf(g_rotationAngle) * cosf(g_pitchAngle) * g_radius;
            float camY = sinf(g_pitchAngle) * g_radius;
            float camZ = cosf(g_rotationAngle) * cosf(g_pitchAngle) * g_radius;

            XMVECTOR eye = XMVectorSet(camX, camY, camZ, 0.0f);
            XMVECTOR focus = XMVectorZero(); // look at cube center
            XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

            XMMATRIX view = XMMatrixLookAtLH(eye, focus, up);

            // --- Mouse input for rotation ---
            static POINT lastMouse = {};
            POINT currMouse;
            GetCursorPos(&currMouse);

            if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)  // right button held
            {
                float dx = (currMouse.x - lastMouse.x) * 0.005f; // sensitivity
                float dy = (currMouse.y - lastMouse.y) * 0.005f;

                g_rotationAngle += dx;   // horizontal orbit
                g_pitchAngle += dy;      // vertical orbit

                // Clamp pitch to avoid flipping
                const float pitchLimit = XM_PIDIV2 - 0.01f;
                if (g_pitchAngle > pitchLimit) g_pitchAngle = pitchLimit;
                if (g_pitchAngle < -pitchLimit) g_pitchAngle = -pitchLimit;
            }

            lastMouse = currMouse;

            D3D11_MAPPED_SUBRESOURCE ms2;
            g_pImmediateContext->Map(g_pMatrixBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms2);
            XMMATRIX* m = (XMMATRIX*)ms2.pData;
            m[0] = XMMatrixTranspose(world);
            m[1] = XMMatrixTranspose(view);
            m[2] = XMMatrixTranspose(g_proj);
            g_pImmediateContext->Unmap(g_pMatrixBuffer, 0);
            g_pImmediateContext->VSSetConstantBuffers(0, 1, &g_pMatrixBuffer);

            // Draw lines
            g_pImmediateContext->Draw(verts.size(), 0);

            // === QUICK FIX: DRAW PARTICLES AS POINTS (add everything below here) ===
            std::vector<Vertex> pointVerts;
            pointVerts.reserve(numParticles);
            for (const auto& p : particles)
            {
                // Simple depth-based brightness (closer = brighter)
                float depthNorm = (p.pos.z - 50.0f) / 150.0f;  // 0 = near, 1 = far
                float brightness = 1.0f - depthNorm * 0.6f;    // keep some minimum brightness
                brightness = std::max(brightness, 0.4f);

                pointVerts.push_back({ p.pos, XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) });  // solid white, full alpha
                // Or use brightness: XMFLOAT4(brightness, brightness, brightness + 0.2f, 1.0f)
            }

            // Update the same vertex buffer with point data
            g_pImmediateContext->Map(g_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            memcpy(ms.pData, pointVerts.data(), pointVerts.size() * sizeof(Vertex));
            g_pImmediateContext->Unmap(g_pVertexBuffer, 0);

            // Switch to drawing points
            g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

            // Draw the particles
            g_pImmediateContext->Draw(numParticles, 0);

            // (Optional) Switch back to lines if you plan to draw more stuff later
            // g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

            // === End of quick fix ===

            // Present
            g_pSwapChain->Present(1, 0);
        }
    }

    return 0;
}

