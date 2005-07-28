import java.io.DataInput;
import java.io.IOException;

public class BdescAlloc extends Opcode
{
	public BdescAlloc(int block, int ddesc, int number)
	{
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_ALLOC, "KDB_BDESC_ALLOC", BdescAlloc.class);
		factory.addParameter("block", 4);
		factory.addParameter("ddesc", 4);
		factory.addParameter("number", 4);
		return factory;
	}
}
