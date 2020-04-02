# Keeping CI images up to date

## Re-building an image after updating the docker file

Run `docker build -t snmallocciteam/$IMG:latest -f ci/$IMG .` from the root of the repo,
where `$IMG` is the image you want to rebuild, for example `build_linux_x64`

If you are building a multiarch image, ie. an image targeting an architecture other than
the one you are running, you will need to install the qemu handler before you can build:
`sudo docker run --rm --privileged multiarch/qemu-user-static:register --reset`

## Pushing the updated docker image to Docker Hub

Run `docker push snmallocciteam/$IMG:latest`

## Permissions

You must be part of the [snmalloc ci team](https://hub.docker.com/orgs/snmallocciteam) to
push updated images. Contact @mjp41, or @achamayou to be given access.
