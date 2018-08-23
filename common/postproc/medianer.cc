#include <algorithm>
#include "postproc/medianer.h"

const char* kMedianerShader[] = {R"glsl(
    uniform mat4 u_view;
    uniform float u_cx;
    uniform float u_cy;
    uniform float u_fx;
    uniform float u_fy;
    attribute vec4 v_vertex;
    attribute vec2 v_coord;
    varying vec2 v_UV;

    void main() {
      v_UV.x = v_coord.x;
      v_UV.y = 1.0 - v_coord.y;
      gl_Position = u_view * v_vertex;
      gl_Position.xy /= abs(gl_Position.z * gl_Position.w);
      gl_Position.x *= u_fx;
      gl_Position.y *= u_fy;
      gl_Position.x += u_cx;
      gl_Position.y += u_cy;
      gl_Position.z *= 0.01;
      gl_Position.w = 1.0;
    })glsl",

    R"glsl(
    uniform sampler2D u_texture;
    varying vec2 v_UV;

    void main() {
      gl_FragColor = texture2D(u_texture, v_UV);
    })glsl"
};

#define DOWNSIZE_FRAME 4
#define DOWNSIZE_TEXTURE 4

namespace oc {

    Medianer::Medianer(std::string path, std::string filename, bool preparePhotos) {
        /// init dataset
        dataset = new oc::Dataset(path);
        dataset->GetCalibration(cx, cy, fx, fy);
        dataset->GetState(poseCount, width, height);
        SetResolution(width / DOWNSIZE_FRAME, height / DOWNSIZE_FRAME);
        currentDepth = new double[viewport_width * viewport_height];
        cx /= (double)width;
        cy /= (double)height;
        fx /= (double)width;
        fy /= (double)height;

        /// init frame rendering
        shader = new oc::GLSL(kMedianerShader[0], kMedianerShader[1]);

        /// load model
        File3d obj(dataset->GetPath() + "/" + filename, false);
        obj.ReadModel(25000, model);
        for (Mesh& m : model) {
            if (m.image && m.imageOwner) {
                m.image->Downsize(DOWNSIZE_TEXTURE);
                memset(m.image->GetData(), 0, m.image->GetWidth() * m.image->GetHeight() * 4);
            }
        }

        /// prepare photos
        if (preparePhotos) {
            for (unsigned int i = 0; i <= poseCount; i++) {
                Image img(dataset->GetFileName(i, ".jpg"));
                img.Downsize(DOWNSIZE_FRAME);
                img.Blur(1);
                img.EdgeDetect();
                img.Blur(1);
                img.Write(dataset->GetFileName(i, ".edg"));
            }
        }

        /// init variables
        currentImage = 0;
        lastIndex = -1;
        modification = glm::mat4(1);
    }

    Medianer::~Medianer() {
        delete[] currentDepth;
        if (currentImage)
            delete currentImage;
        delete dataset;
        delete shader;
    }

    void Medianer::Process(unsigned long& index, int &x1, int &x2, int &y, glm::dvec3 &z1, glm::dvec3 &z2) {
        if ((y >= 0) && (y < viewport_height)) {
            glm::vec4 color;
            int mem;
            int offset = y * viewport_width;
            Image* i = model[currentMesh].image;
            for (int x = glm::max(x1, 0); x <= glm::min(x2, viewport_width - 1); x++) {
                glm::dvec3 z = z1 + (double)(x - x1) * (z2 - z1) / (double)(x2 - x1);
                if ((z.z > 0) && (currentDepth[y * viewport_width + x] >= z.z)) {
                    currentDepth[offset + x] = z.z;

                    //get color from frame
                    if (currentPass != PASS_DEPTH) {
                        z.t = 1.0 - z.t;
                        z.s *= i->GetWidth() - 1;
                        z.t *= i->GetHeight() - 1;
                        mem = (((int)z.t) * i->GetWidth() + (int)z.s) * 4;
                        color = currentImage->GetColorRGBA(x, y, 0);
                    }

                    //count error
                    if (currentPass == PASS_ERROR) {
                        i->GetData()[mem + 1] = abs(i->GetData()[mem + 0] - color.r);
                        if (i->GetData()[mem + 3] > 0)
                            currentError += i->GetData()[mem + 1];
                        currentCount++;
                    }

                    //apply color
                    if (currentPass == PASS_APPLY) {
                        i->GetData()[mem + 0] = color.r;
                        i->GetData()[mem + 3] = 255;
                    }
                }
            }
        }
    }

    void Medianer::RenderPose(int index) {
        glm::mat4 view = glm::inverse(dataset->GetPose(index)[0]);
        shader->Bind();
        shader->UniformMatrix("u_view", glm::value_ptr(view));
        for (Mesh& m : model) {
            if (!m.vertices.empty()) {
                glActiveTexture(GL_TEXTURE0);
                if (m.image && (m.image->GetTexture() == -1))
                    m.image->SetTexture((long) Image2GLTexture(m.image));
                glBindTexture(GL_TEXTURE_2D, m.image->GetTexture());
                shader->UniformInt("u_texture", 0);
                shader->UniformFloat("u_cx", cx - 0.5f);
                shader->UniformFloat("u_cy",-cy + 0.5f);
                shader->UniformFloat("u_fx", 2.0f * fx);
                shader->UniformFloat("u_fy",-2.0f * fy);
                shader->Attrib(&m.vertices[0].x, 0, &m.uv[0].x, 0);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei) m.vertices.size());
            }
        }
        for (Mesh& m : model) {
            if (m.image && (m.image->GetTexture() != -1)) {
                GLuint texture = m.image->GetTexture();
                glDeleteTextures(1, &texture);
                m.image->SetTexture(-1);
            }
        }
    }

    float Medianer::RenderTexture(int index) {
        for (int i = 0; i < viewport_width * viewport_height; i++)
            currentDepth[i] = 9999;
        for (Mesh& m : model) {
            if (m.image && m.imageOwner) {
                for (unsigned int i = 1; i < m.image->GetWidth() * m.image->GetHeight() * 4; i += 4) {
                    m.image->GetData()[i] = 0;
                }
            }
        }

        currentCount = 1;
        currentError = 0;
        if (index != lastIndex) {
            if (currentImage)
                delete currentImage;
            currentImage = new Image(dataset->GetFileName(index, ".edg"));
            lastIndex = index;
        }
        currentPose = modification * glm::inverse(dataset->GetPose(index)[0]);
        for (currentPass = 0; currentPass < PASS_COUNT; currentPass++) {
            if (currentPass == PASS_APPLY) {
                float error = currentError / (float)currentCount / 255.0f;
                if (error > 0.125)
                    return error;
            }
            for (currentMesh = 0; currentMesh < model.size(); currentMesh++)
                AddUVVertices(model[currentMesh].vertices, model[currentMesh].uv, currentPose,
                              cx - 0.5, cy - 0.5, 2.0 * fx, 2.0 * fy);
        }
        return 0;
    }

    void Medianer::SetModification(glm::vec3 rot, glm::vec3 scl, glm::vec3 trn) {
        modification = glm::mat4(1);
        if (fabs(rot.x) > 0.001) modification = glm::rotate(modification, rot.x * (float)M_PI / 20.0f, glm::vec3(1, 0, 0));
        if (fabs(rot.y) > 0.001) modification = glm::rotate(modification, rot.y * (float)M_PI / 20.0f, glm::vec3(0, 1, 0));
        if (fabs(rot.z) > 0.001) modification = glm::rotate(modification, rot.z * (float)M_PI / 20.0f, glm::vec3(0, 0, 1));
        modification = glm::scale(modification, scl);
        modification = glm::translate(modification, trn);
    }

    GLuint Medianer::Image2GLTexture(oc::Image* img) {
        GLuint textureID;
        glGenTextures(1, &textureID);
        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->GetWidth(), img->GetHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img->GetData());
        glGenerateMipmap(GL_TEXTURE_2D);
        return textureID;
    }
}
