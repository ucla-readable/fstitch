import java.io.DataInput;
import java.io.IOException;

public class BdescModule extends Module
{
	public BdescModule(DataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_BDESC);
		
		addFactory(BdescAlloc.getFactory(input));
		addFactory(BdescAllocWrap.getFactory(input));
		addFactory(BdescRetain.getFactory(input));
		addFactory(BdescRelease.getFactory(input));
		addFactory(BdescDestroy.getFactory(input));
		addFactory(BdescFreeDdesc.getFactory(input));
		addFactory(BdescAutoRelease.getFactory(input));
		addFactory(BdescARReset.getFactory(input));
		addFactory(BdescARPoolPush.getFactory(input));
		addFactory(BdescARPoolPop.getFactory(input));
	}
}
