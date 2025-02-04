/*
    Lightmetrica - Copyright (c) 2019 Hisanari Otsu
    Distributed under MIT license. See LICENSE file for details.
*/

#include <pch.h>
#include <lm/core.h>
#include <lm/camera.h>
#include <lm/film.h>

LM_NAMESPACE_BEGIN(LM_NAMESPACE)

/*
\rst
.. function:: camera::pinhole

   Pinhole camera.
   
   :param str film: Underlying film specified by asset name or locator.
   :param vec3 position: Camera position.
   :param vec3 center: Look-at position.
   :param vec3 up: Up vector.
   :param float vfov: Vertical field of view.

   This component implements pinhole camera where all the incoming lights pass through
   an small aperture and projected onto a film in the opposite side of the aperture.
   Unlike real pinhole camera, the apearture is modeled as a point,
   and the film can be placed in front of the pinhole.
   
   The configuration of the pinhole camera is described by a 3-tuple by
   ``position``, ``center``, and ``up`` vector.
   ``position`` represents a position of the pinhole,
   ``center`` for look-at position. This means the camera faces toward
   the direction to ``center`` from ``position``.
   ``up`` describes the upward direction of the camera.

   Field of view (FoV) describe the extent of the viewing angle of the camera.
   In this implementation, the configuration is given by ``vfov`` parameter.
   Note that we adopted vertical FoV. Be careful if you want to convert from
   other tools that might adopt horizontal FoV.
\endrst
*/
class Camera_Pinhole final : public Camera {
private:
    Vec3 position_;   // Camera position
    Vec3 center_;     // Lookat position
    Vec3 up_;         // Up vector

    Vec3 u_, v_, w_;  // Basis for camera coordinates
    Float vfov_;      // Vertical field of view
    Float tf_;        // Half of the screen height at 1 unit forward from the position

public:
    LM_SERIALIZE_IMPL(ar) {
        ar(position_, center_, up_, u_, v_, w_, vfov_, tf_);
    }

public:
    virtual Json underlyingValue(const std::string&) const override {
        return {
            {"eye", position_},
            {"center", center_},
            {"up", up_},
            {"vfov", vfov_}
        };
    }

    virtual bool construct(const Json& prop) override {
        position_ = json::value<Vec3>(prop, "position"); // Camera position
        center_ = json::value<Vec3>(prop, "center");     // Look-at position
        up_ = json::value<Vec3>(prop, "up");             // Up vector
        vfov_ = json::value<Float>(prop, "vfov");        // Vertical FoV
        tf_ = tan(vfov_ * Pi / 180_f * .5_f);            // Precompute half of screen height
        w_ = glm::normalize(position_ - center_);        // Compute basis
        u_ = glm::normalize(glm::cross(up_, w_));
        v_ = cross(w_, u_);
        return true;
    }

    virtual bool isSpecular(const PointGeometry&) const override {
        return false;
    }

    virtual Ray primaryRay(Vec2 rp, Float aspectRatio) const override {
        rp = 2_f*rp-1_f;
        const auto d = glm::normalize(Vec3(aspectRatio*tf_*rp.x, tf_*rp.y, -1_f));
        return { position_, u_*d.x+v_*d.y+w_*d.z };
    }

    virtual std::optional<Vec2> rasterPosition(Vec3 wo, Float aspectRatio) const override {
        // Convert to camera space
        const auto toEye = glm::transpose(Mat3(u_, v_, w_));
        const auto woEye = toEye * wo;
        if (woEye.z >= 0) {
            // wo is directed to the opposition direction
            return {};
        }

        // Calculate raster position
        const auto rp = Vec2(
            -woEye.x/woEye.z/tf_/aspectRatio,
            -woEye.y/woEye.z/tf_)*.5_f + .5_f;
        if (rp.x < 0_f || rp.x > 1_f || rp.y < 0_f || rp.y > 1_f) {
            // wo is not in the view frustum
            return {};
        }
        
        return rp;
    }

    virtual std::optional<CameraRaySample> samplePrimaryRay(Rng& rng, Vec4 window, Float aspectRatio) const override {
        const auto [x, y, w, h] = window.data.data;
        return CameraRaySample{
            PointGeometry::makeDegenerated(position_),
            primaryRay({x+w*rng.u(), y+h*rng.u()}, aspectRatio).d,
            Vec3(1_f)
        };
    }

    virtual Float pdf(Vec3 wo, Float aspectRatio) const override {
        // Given directions is not samplable if raster position is not in [0,1]^2
        if (!rasterPosition(wo, aspectRatio)) {
            return 0_f;
        }
        return J(wo, aspectRatio);
    }

    virtual Vec3 eval(Vec3 wo, Float aspectRatio) const override {
        if (!rasterPosition(wo, aspectRatio)) {
            return Vec3(0_f);
        }
        return Vec3(J(wo, aspectRatio));
    }

    virtual Mat4 viewMatrix() const override {
        return glm::lookAt(position_, position_ - w_, up_);
    }

    virtual Mat4 projectionMatrix(Float aspectRatio) const override {
        return glm::perspective(glm::radians(vfov_), aspectRatio, 0.01_f, 10000_f);
    }

private:
    // Compute Jacobian
    // TODO. Add derivation in documentataion
    Float J(Vec3 wo, Float aspectRatio) const {
        const auto V = glm::transpose(Mat3(u_, v_, w_));
        const auto woEye = V * wo;
        const Float cosTheta = -woEye.z;
        const Float invCosTheta = 1_f / cosTheta;
        const Float A = tf_ * tf_ * aspectRatio * 4_f;
        return invCosTheta * invCosTheta * invCosTheta / A;
    }
};

LM_COMP_REG_IMPL(Camera_Pinhole, "camera::pinhole");

LM_NAMESPACE_END(LM_NAMESPACE)
