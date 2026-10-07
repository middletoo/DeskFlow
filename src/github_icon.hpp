#pragma once
#include <d2d1.h>
#include <wrl/client.h>

namespace desk {
inline constexpr wchar_t githubProjectUrl[]=L"https://github.com/middletoo/DeskFlow";
// GitHub mark from Simple Icons (CC0), converted from the vendored SVG path.
inline HRESULT makeGithubGeometry(ID2D1Factory* factory,ID2D1PathGeometry** output){
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    HRESULT result=factory->CreatePathGeometry(&geometry);if(FAILED(result))return result;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    result=geometry->Open(&sink);if(FAILED(result))return result;
    sink->BeginFigure({12.0f,0.297f},D2D1_FIGURE_BEGIN_FILLED);
    static constexpr D2D1_BEZIER_SEGMENT segments[]{
        {{5.37f,0.297f},{0.0f,5.67f},{0.0f,12.297f}},
        {{0.0f,17.6f},{3.438f,22.097f},{8.205f,23.682f}},
        {{8.805f,23.795f},{9.025f,23.424f},{9.025f,23.105f}},
        {{9.025f,22.82f},{9.015f,22.065f},{9.01f,21.065f}},
        {{5.672f,21.789f},{4.968f,19.455f},{4.968f,19.455f}},
        {{4.422f,18.07f},{3.633f,17.7f},{3.633f,17.7f}},
        {{2.546f,16.956f},{3.717f,16.971f},{3.717f,16.971f}},
        {{4.922f,17.055f},{5.555f,18.207f},{5.555f,18.207f}},
        {{6.625f,20.042f},{8.364f,19.512f},{9.05f,19.205f}},
        {{9.158f,18.429f},{9.467f,17.9f},{9.81f,17.6f}},
        {{7.145f,17.3f},{4.344f,16.268f},{4.344f,11.67f}},
        {{4.344f,10.36f},{4.809f,9.29f},{5.579f,8.45f}},
        {{5.444f,8.147f},{5.039f,6.927f},{5.684f,5.274f}},
        {{5.684f,5.274f},{6.689f,4.952f},{8.984f,6.504f}},
        {{9.944f,6.237f},{10.964f,6.105f},{11.984f,6.099f}},
        {{13.004f,6.105f},{14.024f,6.237f},{14.984f,6.504f}},
        {{17.264f,4.952f},{18.269f,5.274f},{18.269f,5.274f}},
        {{18.914f,6.927f},{18.509f,8.147f},{18.389f,8.45f}},
        {{19.154f,9.29f},{19.619f,10.36f},{19.619f,11.67f}},
        {{19.619f,16.28f},{16.814f,17.295f},{14.144f,17.59f}},
        {{14.564f,17.95f},{14.954f,18.686f},{14.954f,19.81f}},
        {{14.954f,21.416f},{14.939f,22.706f},{14.939f,23.096f}},
        {{14.939f,23.411f},{15.149f,23.786f},{15.764f,23.666f}},
        {{20.565f,22.092f},{24.0f,17.592f},{24.0f,12.297f}},
        {{24.0f,5.67f},{18.627f,0.297f},{12.0f,0.297f}},
    };
    sink->AddBeziers(segments,(UINT32)(sizeof(segments)/sizeof(segments[0])));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);result=sink->Close();if(FAILED(result))return result;
    *output=geometry.Detach();return S_OK;
}
}
