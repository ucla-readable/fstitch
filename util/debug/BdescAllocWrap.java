import java.io.DataInput;
import java.io.IOException;

public class BdescAllocWrap extends Opcode
{
	public BdescAllocWrap(int block, int ddesc, int number)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_ALLOC_WRAP, "KDB_BDESC_ALLOC_WRAP", BdescAllocWrap.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("number", 4);
		return factory;
	}
}
