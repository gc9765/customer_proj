.PHONY: clean All Project_Title Project_PreBuild Project_Build Project_PostBuild

All: Project_Title Project_PreBuild Project_Build Project_PostBuild

Project_Title:
	@echo "----------Building project:[ fpv_app_umac4 - FLASH ]----------"

Project_PreBuild:
	@echo Executing Pre Build commands ...
	@export CDKPath="D:/C-Sky/CDK" CDK_VERSION="V2.10.3" ProjectPath="D:/GUQIAO3/36533_1111_intercom - SERVER/project/" && "D:/GUQIAO3/36533_1111_intercom - SERVER/project/prebuild.sh" $< 
	@echo Done

Project_Build:
	@make -r -f fpv_app_umac4.mk -j 4 -C  ./ 

Project_PostBuild:
	@echo Executing Post Build commands ...
	@export CDKPath="D:/C-Sky/CDK" CDK_VERSION="V2.10.3" ProjectPath="D:/GUQIAO3/36533_1111_intercom - SERVER/project/" && "D:/GUQIAO3/36533_1111_intercom - SERVER/project/BuildBIN.sh" 
	@echo Done


clean:
	@echo "----------Cleaning project:[ fpv_app_umac4 - FLASH ]----------"

