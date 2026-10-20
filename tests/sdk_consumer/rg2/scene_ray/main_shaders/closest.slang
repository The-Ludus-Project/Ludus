// Data/query acceptance shader, deliberately without a lighting estimator.
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    MaterialSample material = EvaluateMaterial(scene, hit);
    payload.Radiance = material.BaseColor + EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
    SelectedLight selected = SelectLight(scene, payload.Launch, 2);
    LightSample light = SampleIncidentLight(scene, selected, hit, float3(0.5, 0.5, 0.5));
    if (light.HasSample == true)
    {
        // Validate the proposal/evaluation contract without estimating illumination.
        if (light.IsDiscrete == false)
        {
            float evaluatedPdf;
            LightInfo source = GetLightInfo(scene, light.LightIndex);
            if (source.Kind == SCENE_LIGHT_MESH)
            {
                SurfaceHit endpoint = GetSurfaceHit(scene, light.TriangleIndex, light.Barycentric);
                evaluatedPdf = EvaluateLightPdfSolidAngle(scene, hit.PositionWorld, endpoint);
            }
            else
            {
                evaluatedPdf = EvaluateEnvironmentPdfSolidAngle(scene, light.DirectionWorld);
            }
            const float PDF_RELATIVE_TOLERANCE = 0.0001;
            float samplePdf = light.SelectionPmf * light.ConditionalPdf;
            if (abs(evaluatedPdf - samplePdf) > PDF_RELATIVE_TOLERANCE * max(evaluatedPdf, samplePdf))
                queries.HasFailed = true;
        }
        uint visibility = TestVisibility(scene, queries, hit, light);
        if (visibility == SCENE_QUERY_FAILURE || light.SelectionPmf <= 0 || light.ConditionalPdf <= 0)
            queries.HasFailed = true;
    }
}
